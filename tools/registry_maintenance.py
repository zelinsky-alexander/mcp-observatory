#!/usr/bin/env python3
"""Crash-safe local catalog refresh, backup, and restore orchestration."""

from __future__ import annotations

import argparse
import contextlib
import datetime as dt
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import sqlite3
import subprocess
import sys
import urllib.parse
import uuid
from zoneinfo import ZoneInfo


STATUS_SCHEMA_VERSION = 1
DEFAULT_BACKUP_RETENTION_COUNT = 2


class MaintenanceError(RuntimeError):
    def __init__(self, category: str, message: str, exit_code: int = 1):
        super().__init__(message)
        self.category = category
        self.exit_code = exit_code


def utc_now() -> dt.datetime:
    return dt.datetime.now(dt.timezone.utc).replace(microsecond=0)


def timestamp(value: dt.datetime) -> str:
    return value.isoformat().replace("+00:00", "Z")


def stamp(value: dt.datetime) -> str:
    return value.strftime("%Y%m%dT%H%M%SZ")


def fsync_file(path: Path) -> None:
    descriptor = os.open(path, os.O_RDONLY | os.O_CLOEXEC)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def fsync_directory(path: Path) -> None:
    descriptor = os.open(path, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def atomic_json(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    temporary = path.with_name(f".{path.name}.{uuid.uuid4().hex}.tmp")
    try:
        with temporary.open("x", encoding="utf-8") as output:
            json.dump(value, output, sort_keys=True, separators=(",", ":"))
            output.write("\n")
            output.flush()
            os.fsync(output.fileno())
        os.chmod(temporary, 0o600)
        os.replace(temporary, path)
        fsync_directory(path.parent)
    finally:
        with contextlib.suppress(OSError):
            temporary.unlink(missing_ok=True)


@contextlib.contextmanager
def writer_lock(database: Path):
    lock_path = Path(f"{database}.writer.lock")
    lock_path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    descriptor = os.open(
        lock_path,
        os.O_RDWR | os.O_CREAT | os.O_CLOEXEC | os.O_NOFOLLOW,
        0o600,
    )
    try:
        fcntl.flock(descriptor, fcntl.LOCK_EX)
        yield
    finally:
        os.close(descriptor)


def connect_readonly(path: Path) -> sqlite3.Connection:
    quoted = urllib.parse.quote(str(path.resolve()), safe="/")
    return sqlite3.connect(f"file:{quoted}?mode=ro", uri=True, timeout=5)


def validate_database(path: Path, expected_digest: str | None = None) -> dict:
    if not path.is_file():
        raise MaintenanceError("database_missing", f"database is not a regular file: {path}")
    try:
        with connect_readonly(path) as connection:
            integrity = [row[0] for row in connection.execute("PRAGMA integrity_check")]
            if integrity != ["ok"]:
                raise MaintenanceError(
                    "integrity_failure", "SQLite integrity_check did not return ok"
                )
            foreign_keys = list(connection.execute("PRAGMA foreign_key_check"))
            if foreign_keys:
                raise MaintenanceError(
                    "integrity_failure", "SQLite foreign_key_check returned violations"
                )
            latest = connection.execute(
                "SELECT snapshot_sha256,pages,records_received,"
                "unique_server_versions FROM snapshots "
                "ORDER BY completed_at DESC,id DESC LIMIT 1"
            ).fetchone()
            if latest is None:
                raise MaintenanceError(
                    "validation_failure", "database has no completed snapshot"
                )
            if expected_digest is not None and latest[0] != expected_digest:
                raise MaintenanceError(
                    "validation_failure",
                    "staged latest snapshot does not match the collected bundle",
                )
            counts = {
                "catalog_snapshots": connection.execute(
                    "SELECT COUNT(*) FROM snapshots"
                ).fetchone()[0],
                "catalog_server_versions": connection.execute(
                    "SELECT COUNT(*) FROM server_versions"
                ).fetchone()[0],
                "catalog_snapshot_links": connection.execute(
                    "SELECT COUNT(*) FROM snapshot_server_versions"
                ).fetchone()[0],
                "completed_pages": latest[1],
                "received_records": latest[2],
                "unique_server_versions": latest[3],
            }
            return {"snapshot_digest": latest[0], "counts": counts}
    except MaintenanceError:
        raise
    except sqlite3.Error as error:
        raise MaintenanceError("integrity_failure", f"SQLite validation failed: {error}") from error


def sqlite_backup(source: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    destination.unlink(missing_ok=True)
    try:
        with connect_readonly(source) as source_connection:
            with sqlite3.connect(destination) as destination_connection:
                source_connection.backup(destination_connection)
        os.chmod(destination, 0o600)
        fsync_file(destination)
    except sqlite3.Error as error:
        destination.unlink(missing_ok=True)
        raise MaintenanceError("backup_failure", f"SQLite backup failed: {error}") from error


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while block := source.read(1024 * 1024):
            digest.update(block)
    return digest.hexdigest()


def publish_backup(source: Path, destination: Path) -> dict:
    temporary = destination.with_name(f".{destination.name}.{uuid.uuid4().hex}.tmp")
    try:
        sqlite_backup(source, temporary)
        details = validate_database(temporary)
        digest = sha256_file(temporary)
        os.replace(temporary, destination)
        fsync_directory(destination.parent)
        atomic_json(
            Path(f"{destination}.json"),
            {
                "schema_version": 1,
                "database_sha256": digest,
                "database_bytes": destination.stat().st_size,
                "created_at_utc": timestamp(utc_now()),
                "snapshot_digest": details["snapshot_digest"],
                "counts": details["counts"],
            },
        )
        return details | {"database_sha256": digest}
    finally:
        with contextlib.suppress(OSError):
            temporary.unlink(missing_ok=True)


def prune_refresh_backups(
    backup_directory: Path, database_name: str, retention_count: int
) -> None:
    name_pattern = re.compile(
        re.escape(database_name)
        + r"\.\d{8}T\d{6}Z-[0-9a-f]{8}\.sqlite\Z"
    )
    backups = sorted(
        entry
        for entry in backup_directory.iterdir()
        if name_pattern.fullmatch(entry.name) is not None
        and (entry.is_file() or entry.is_symlink())
    )
    removed = False
    for backup in backups[:-retention_count]:
        Path(f"{backup}.json").unlink(missing_ok=True)
        backup.unlink()
        removed = True
    if removed:
        fsync_directory(backup_directory)


def positive_integer(value: str) -> int:
    parsed = int(value)
    if parsed < 1:
        raise argparse.ArgumentTypeError("must be at least 1")
    return parsed


def load_status(path: Path) -> dict:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
        return value if isinstance(value, dict) else {}
    except (FileNotFoundError, OSError, json.JSONDecodeError):
        return {}


def status_base(started: dt.datetime, timezone: str) -> dict:
    return {
        "schema_version": STATUS_SCHEMA_VERSION,
        "timezone": timezone,
        "attempt": {
            "started_at_utc": timestamp(started),
            "started_at_local": started.astimezone(ZoneInfo(timezone)).isoformat(),
        },
    }


def write_failure_status(
    path: Path,
    started: dt.datetime,
    timezone: str,
    error: MaintenanceError,
    attempt_details: dict,
) -> None:
    failed = utc_now()
    previous = load_status(path)
    value = status_base(started, timezone)
    value["attempt"].update(attempt_details)
    value.update(
        {
            "state": "failed",
            "last_success": previous.get("last_success"),
            "failure": {
                "category": error.category,
                "message": str(error),
                "exit_code": error.exit_code,
                "failed_at_utc": timestamp(failed),
                "failed_at_local": failed.astimezone(ZoneInfo(timezone)).isoformat(),
            },
        }
    )
    atomic_json(path, value)


def run_checked(command: list[str], category: str, stdout_path: Path, stderr_path: Path) -> str:
    with stdout_path.open("w", encoding="utf-8") as stdout, stderr_path.open(
        "w", encoding="utf-8"
    ) as stderr:
        result = subprocess.run(command, stdout=stdout, stderr=stderr, check=False)
    if result.returncode != 0:
        detail = stderr_path.read_text(encoding="utf-8", errors="replace")[-4096:].strip()
        raise MaintenanceError(
            category,
            f"command failed with exit code {result.returncode}: {detail}",
            result.returncode,
        )
    return stdout_path.read_text(encoding="utf-8")


def refresh(args: argparse.Namespace) -> int:
    database = args.database.resolve()
    runtime = args.runtime_dir.resolve()
    status_path = runtime / "last-success.json"
    started = utc_now()
    token = f"{stamp(started)}-{uuid.uuid4().hex[:8]}"
    bundle = runtime / "bundles" / f"official-refresh-{token}"
    logs = runtime / "logs"
    backup = runtime / "backups" / f"{database.name}.{token}.sqlite"
    stage = database.with_name(f".{database.name}.{uuid.uuid4().hex}.staging")
    result_log = logs / f"official-refresh-{token}.result.json"
    progress_log = logs / f"official-refresh-{token}.progress.log"
    validate_log = logs / f"official-refresh-{token}.validate.log"
    attempt_details = {
        "bundle": str(bundle),
        "result_log": str(result_log),
        "progress_log": str(progress_log),
    }
    for directory in (bundle.parent, logs, backup.parent):
        directory.mkdir(parents=True, exist_ok=True, mode=0o700)
    try:
        if not args.binary.is_file() or not os.access(args.binary, os.X_OK):
            raise MaintenanceError("binary_missing", f"binary is not executable: {args.binary}")
        with writer_lock(database):
            validate_database(database)
            sqlite_backup(database, stage)
            command = [
                str(args.binary.resolve()),
                "registry",
                "refresh",
                "--database",
                str(stage),
                "--output",
                str(bundle),
                "--format",
                "json",
                *(args.refresh_arguments[1:]
                  if args.refresh_arguments[:1] == ["--"]
                  else args.refresh_arguments),
            ]
            try:
                output = run_checked(command, "refresh_failure", result_log, progress_log)
            except MaintenanceError as error:
                if (bundle.joinpath("_SUCCESS").is_file()):
                    error.category = "import_failure"
                else:
                    error.category = "collection_failure"
                raise
            if not bundle.joinpath("_SUCCESS").is_file():
                raise MaintenanceError("collection_failure", "completed refresh has no _SUCCESS marker")
            validation_output = logs / f"official-refresh-{token}.bundle-validation.txt"
            run_checked(
                [str(args.binary.resolve()), "bundle", "validate", str(bundle)],
                "bundle_validation_failure",
                validation_output,
                validate_log,
            )
            try:
                refresh_result = json.loads(output)
            except json.JSONDecodeError as error:
                raise MaintenanceError(
                    "validation_failure", f"refresh result is not valid JSON: {error}"
                ) from error
            digest = refresh_result.get("snapshot_sha256")
            if not isinstance(digest, str) or len(digest) != 64:
                raise MaintenanceError("validation_failure", "refresh result has no snapshot digest")
            staged = validate_database(stage, digest)
            if staged["counts"]["completed_pages"] != refresh_result.get("completed_pages"):
                raise MaintenanceError("validation_failure", "staged page count does not match result")
            if staged["counts"]["received_records"] != refresh_result.get("received_records"):
                raise MaintenanceError("validation_failure", "staged record count does not match result")
            publish_backup(database, backup)
            try:
                prune_refresh_backups(
                    backup.parent, database.name, args.backup_retention_count
                )
            except OSError as error:
                raise MaintenanceError(
                    "retention_failure", f"backup retention failed: {error}"
                ) from error
            fsync_file(stage)
            os.replace(stage, database)
            fsync_directory(database.parent)
            completed = utc_now()
            last_success = {
                "started_at_utc": timestamp(started),
                "started_at_local": started.astimezone(
                    ZoneInfo(args.timezone)
                ).isoformat(),
                "completed_at_utc": timestamp(completed),
                "completed_at_local": completed.astimezone(
                    ZoneInfo(args.timezone)
                ).isoformat(),
                "published_at_utc": timestamp(completed),
                "published_at_local": completed.astimezone(
                    ZoneInfo(args.timezone)
                ).isoformat(),
                "snapshot_digest": digest,
                "counts": staged["counts"],
                "bundle": str(bundle),
                "backup": str(backup),
                "result_log": str(result_log),
                "progress_log": str(progress_log),
            }
            value = status_base(started, args.timezone)
            value["attempt"].update(attempt_details)
            value.update({"state": "succeeded", "last_success": last_success, "failure": None})
            atomic_json(status_path, value)
        print(json.dumps(last_success, sort_keys=True, separators=(",", ":")))
        return 0
    except (MaintenanceError, OSError) as error:
        if not isinstance(error, MaintenanceError):
            error = MaintenanceError("local_io_failure", str(error))
        try:
            write_failure_status(
                status_path, started, args.timezone, error, attempt_details
            )
        except OSError as status_error:
            print(f"status_write_failure: {status_error}", file=sys.stderr)
        print(f"{error.category}: {error}", file=sys.stderr)
        return error.exit_code
    finally:
        with contextlib.suppress(OSError):
            stage.unlink(missing_ok=True)
        with contextlib.suppress(OSError):
            Path(f"{stage}.writer.lock").unlink(missing_ok=True)


def backup_command(args: argparse.Namespace) -> int:
    try:
        with writer_lock(args.database.resolve()):
            details = publish_backup(args.database.resolve(), args.output.resolve())
        print(json.dumps(details, sort_keys=True, separators=(",", ":")))
        return 0
    except (MaintenanceError, OSError) as error:
        if not isinstance(error, MaintenanceError):
            error = MaintenanceError("backup_failure", str(error))
        print(f"{error.category}: {error}", file=sys.stderr)
        return error.exit_code


def restore_command(args: argparse.Namespace) -> int:
    database = args.database.resolve()
    source = args.backup.resolve()
    stage = database.with_name(f".{database.name}.{uuid.uuid4().hex}.restore")
    try:
        metadata_path = Path(f"{source}.json")
        if not metadata_path.is_file():
            raise MaintenanceError("backup_verification_failure", f"backup metadata is missing: {metadata_path}")
        try:
            metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as error:
            raise MaintenanceError("backup_verification_failure", f"invalid backup metadata: {error}") from error
        database_digest = metadata.get("database_sha256")
        snapshot_digest = metadata.get("snapshot_digest")
        if (
            metadata.get("schema_version") != 1
            or not isinstance(database_digest, str)
            or len(database_digest) != 64
            or not isinstance(snapshot_digest, str)
            or len(snapshot_digest) != 64
            or metadata.get("database_bytes") != source.stat().st_size
        ):
            raise MaintenanceError(
                "backup_verification_failure", "backup metadata fields are invalid"
            )
        if sha256_file(source) != database_digest:
            raise MaintenanceError("backup_verification_failure", "backup SHA-256 does not match metadata")
        validate_database(source, snapshot_digest)
        sqlite_backup(source, stage)
        validate_database(stage, snapshot_digest)
        with writer_lock(database):
            try:
                validate_database(database)
            except MaintenanceError as validation_error:
                if validation_error.category not in {
                    "database_missing",
                    "integrity_failure",
                    "validation_failure",
                }:
                    raise
                warning = (
                    "pre-restore backup skipped because the live database is "
                    f"invalid: {validation_error.category}: {validation_error}"
                )
                print(f"warning: {warning}", file=sys.stderr)
                pre_restore_backup = {
                    "state": "skipped",
                    "warning": warning,
                    "live_database_validation": {
                        "category": validation_error.category,
                        "message": str(validation_error),
                    },
                }
            else:
                recovery_token = f"{stamp(utc_now())}-{uuid.uuid4().hex[:8]}"
                recovery = database.with_name(
                    f"{database.name}.pre-restore-{recovery_token}.sqlite"
                )
                recovery_metadata = Path(f"{recovery}.json")
                if recovery.exists() or recovery_metadata.exists():
                    raise MaintenanceError(
                        "backup_failure",
                        f"pre-restore backup destination already exists: {recovery}",
                    )
                publish_backup(database, recovery)
                pre_restore_backup = {
                    "state": "created",
                    "path": str(recovery),
                    "metadata_path": str(recovery_metadata),
                }
            fsync_file(stage)
            os.replace(stage, database)
            fsync_directory(database.parent)
        print(
            json.dumps(
                {
                    "state": "restored",
                    "snapshot_digest": snapshot_digest,
                    "pre_restore_backup": pre_restore_backup,
                },
                sort_keys=True,
                separators=(",", ":"),
            )
        )
        return 0
    except (MaintenanceError, OSError) as error:
        if not isinstance(error, MaintenanceError):
            error = MaintenanceError("restore_failure", str(error))
        print(f"{error.category}: {error}", file=sys.stderr)
        return error.exit_code
    finally:
        with contextlib.suppress(OSError):
            stage.unlink(missing_ok=True)


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser()
    subcommands = result.add_subparsers(dest="command", required=True)
    refresh_parser = subcommands.add_parser("refresh")
    refresh_parser.add_argument("--binary", type=Path, required=True)
    refresh_parser.add_argument("--database", type=Path, required=True)
    refresh_parser.add_argument("--runtime-dir", type=Path, required=True)
    refresh_parser.add_argument("--timezone", default="UTC")
    refresh_parser.add_argument(
        "--backup-retention-count",
        type=positive_integer,
        default=DEFAULT_BACKUP_RETENTION_COUNT,
    )
    refresh_parser.add_argument("refresh_arguments", nargs=argparse.REMAINDER)
    refresh_parser.set_defaults(handler=refresh)
    backup_parser = subcommands.add_parser("backup")
    backup_parser.add_argument("--database", type=Path, required=True)
    backup_parser.add_argument("--output", type=Path, required=True)
    backup_parser.set_defaults(handler=backup_command)
    restore_parser = subcommands.add_parser("restore")
    restore_parser.add_argument("--database", type=Path, required=True)
    restore_parser.add_argument("--backup", type=Path, required=True)
    restore_parser.set_defaults(handler=restore_command)
    return result


if __name__ == "__main__":
    os.umask(0o077)
    arguments = parser().parse_args()
    raise SystemExit(arguments.handler(arguments))
