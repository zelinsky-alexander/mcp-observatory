#!/usr/bin/env python3
"""Offline reliability tests for catalog maintenance orchestration."""

import fcntl
import hashlib
import http.server
import json
import os
from pathlib import Path
import runpy
import shutil
import signal
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
from types import SimpleNamespace
import urllib.parse


BASE_ENTRY = {
    "server": {
        "name": "io.example/baseline",
        "version": "1.0.0",
        "description": "baseline",
    },
    "_meta": {
        "io.modelcontextprotocol.registry/official": {
            "isLatest": True,
            "publishedAt": "2026-07-20T00:00:00Z",
            "updatedAt": "2026-07-20T00:00:00Z",
            "status": "active",
        }
    },
}


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, _format, *_args):
        pass

    def do_GET(self):
        parsed = urllib.parse.urlsplit(self.path)
        if parsed.path == "/baseline/v0.1/servers":
            entries = [BASE_ENTRY]
        elif parsed.path == "/refresh/v0.1/servers":
            changed = json.loads(json.dumps(BASE_ENTRY))
            changed["server"].update(
                {"name": "io.example/incremental", "description": "new"}
            )
            entries = [changed]
        else:
            self.send_error(404)
            return
        body = json.dumps(
            {"servers": entries, "metadata": {"nextCursor": None}},
            separators=(",", ":"),
        ).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def run(command, *, env=None, timeout=30):
    return subprocess.run(
        [str(value) for value in command],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
        timeout=timeout,
        env=env,
    )


def require(condition, message, result=None):
    if condition:
        return
    detail = ""
    if result is not None:
        detail = f"\nstdout: {result.stdout}\nstderr: {result.stderr}"
    raise AssertionError(message + detail)


def file_digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def snapshot_count(path):
    with sqlite3.connect(path) as connection:
        return connection.execute("SELECT COUNT(*) FROM snapshots").fetchone()[0]


def create_baseline(binary, root, base):
    bundle = root / "baseline-bundle"
    database = root / "catalog.sqlite"
    collected = run(
        [
            binary,
            "registry",
            "collect",
            "--registry-base-url",
            base + "/baseline",
            "--output",
            bundle,
            "--maximum-attempts-per-page",
            "1",
        ]
    )
    require(collected.returncode == 0, "baseline collection failed", collected)
    indexed = run(
        [binary, "registry", "index", "--bundle", bundle, "--database", database]
    )
    require(indexed.returncode == 0, "baseline import failed", indexed)
    return bundle, database


def maintenance_command(tool, binary, database, runtime, base):
    return [
        sys.executable,
        tool,
        "refresh",
        "--binary",
        binary,
        "--database",
        database,
        "--runtime-dir",
        runtime,
        "--timezone",
        "UTC",
        "--",
        "--registry-base-url",
        base + "/refresh",
        "--maximum-attempts-per-page",
        "1",
        "--run-timeout-seconds",
        "10",
    ]


def make_wrapper(root, binary):
    wrapper = root / "test-binary"
    wrapper.write_text(
        """#!/usr/bin/env python3
import os
import subprocess
import sys
import time

real = os.environ["MCPO_TEST_REAL_BINARY"]
mode = os.environ.get("MCPO_TEST_MODE", "pass")
if sys.argv[1:3] == ["registry", "refresh"]:
    if mode == "fail":
        print("injected collection failure", file=sys.stderr)
        raise SystemExit(47)
    if mode == "sleep":
        time.sleep(20)
    result = subprocess.run([real, *sys.argv[1:]], check=False)
    if mode == "corrupt" and result.returncode == 0:
        database = sys.argv[sys.argv.index("--database") + 1]
        with open(database, "wb") as output:
            output.write(b"not sqlite")
    raise SystemExit(result.returncode)
os.execv(real, [real, *sys.argv[1:]])
""",
        encoding="utf-8",
    )
    wrapper.chmod(0o700)
    return wrapper


def main():
    binary = Path(sys.argv[1]).resolve()
    tool = Path(sys.argv[2]).resolve()
    root = Path(tempfile.mkdtemp(prefix="mcpo-maintenance-test-"))
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        maintenance = runpy.run_path(str(tool))
        parsed_default = maintenance["parser"]().parse_args(
            [
                "refresh",
                "--binary",
                str(binary),
                "--database",
                "catalog.sqlite",
                "--runtime-dir",
                "runtime",
            ]
        )
        require(
            parsed_default.backup_retention_count == 2,
            "default backup retention is not two",
        )
        parsed_custom = maintenance["parser"]().parse_args(
            [
                "refresh",
                "--binary",
                str(binary),
                "--database",
                "catalog.sqlite",
                "--runtime-dir",
                "runtime",
                "--backup-retention-count",
                "1",
            ]
        )
        require(
            parsed_custom.backup_retention_count == 1,
            "configured backup retention was not parsed",
        )

        retention_root = root / "retention-unit"
        retention_root.mkdir()
        retention_names = [
            "catalog[1].sqlite.20260103T000000Z-00000003.sqlite",
            "catalog[1].sqlite.20260101T000000Z-00000001.sqlite",
            "catalog[1].sqlite.20260102T000000Z-00000002.sqlite",
        ]
        for name in retention_names:
            retention_root.joinpath(name).write_bytes(b"backup")
            retention_root.joinpath(f"{name}.json").write_text("{}\n", encoding="utf-8")
        malformed_backup = retention_root / "catalog[1].sqlite.not-a-stamp.sqlite"
        malformed_backup.write_bytes(b"leave me")
        maintenance["prune_refresh_backups"](
            retention_root, "catalog[1].sqlite", 1
        )
        require(
            retention_root.joinpath(retention_names[0]).is_file(),
            "configured retention removed the newest backup",
        )
        require(
            all(
                not retention_root.joinpath(name).exists()
                and not retention_root.joinpath(f"{name}.json").exists()
                for name in retention_names[1:]
            ),
            "configured retention did not remove old backup pairs",
        )
        require(malformed_backup.is_file(), "retention removed a malformed backup name")

        base = f"http://127.0.0.1:{server.server_port}"
        bundle, database = create_baseline(binary, root, base)
        original_digest = file_digest(database)

        # Native writers and maintenance use the same serialized lock.
        lock_path = Path(f"{database}.writer.lock")
        with lock_path.open("a+b") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            waiting = subprocess.Popen(
                [
                    binary,
                    "registry",
                    "index",
                    "--bundle",
                    bundle,
                    "--database",
                    database,
                ],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            time.sleep(0.3)
            require(waiting.poll() is None, "catalog writer did not wait for shared lock")
            fcntl.flock(lock, fcntl.LOCK_UN)
        stdout, stderr = waiting.communicate(timeout=10)
        require(
            waiting.returncode == 0,
            f"serialized writer failed\nstdout: {stdout}\nstderr: {stderr}",
        )

        wrapper = make_wrapper(root, binary)
        environment = os.environ.copy()
        environment["MCPO_TEST_REAL_BINARY"] = str(binary)

        failed_runtime = root / "failed-runtime"
        failed_backup = (
            failed_runtime
            / "backups"
            / f"{database.name}.20000101T000000Z-00000000.sqlite"
        )
        failed_backup.parent.mkdir(parents=True)
        failed_backup.write_bytes(b"existing backup")
        failed_environment = environment | {"MCPO_TEST_MODE": "fail"}
        failed = run(
            maintenance_command(tool, wrapper, database, failed_runtime, base),
            env=failed_environment,
        )
        require(failed.returncode == 47, "staging failure exit was not preserved", failed)
        require(failed_backup.is_file(), "failed refresh pruned an existing backup")
        require(file_digest(database) == original_digest, "staging failure changed live database")
        failed_status = json.loads(
            (failed_runtime / "last-success.json").read_text(encoding="utf-8")
        )
        require(
            failed_status["state"] == "failed"
            and failed_status["failure"]["category"] == "collection_failure",
            "collection failure status is incomplete",
        )

        corrupt_runtime = root / "corrupt-runtime"
        corrupt = run(
            maintenance_command(tool, wrapper, database, corrupt_runtime, base),
            env=environment | {"MCPO_TEST_MODE": "corrupt"},
        )
        require(corrupt.returncode != 0, "corrupt staged database was published", corrupt)
        require(file_digest(database) == original_digest, "integrity failure changed live database")
        corrupt_status = json.loads(
            (corrupt_runtime / "last-success.json").read_text(encoding="utf-8")
        )
        require(
            corrupt_status["failure"]["category"] == "integrity_failure",
            "integrity failure status category is wrong",
        )

        interrupted_runtime = root / "interrupted-runtime"
        interrupted = subprocess.Popen(
            maintenance_command(tool, wrapper, database, interrupted_runtime, base),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=environment | {"MCPO_TEST_MODE": "sleep"},
            start_new_session=True,
        )
        time.sleep(0.5)
        os.killpg(interrupted.pid, signal.SIGTERM)
        interrupted.communicate(timeout=10)
        require(file_digest(database) == original_digest, "interruption changed live database")

        success_runtime = root / "success-runtime"
        success_backup_directory = success_runtime / "backups"
        success_backup_directory.mkdir(parents=True)
        old_backup_names = [
            f"{database.name}.20000101T000000Z-00000000.sqlite",
            f"{database.name}.20010101T000000Z-00000001.sqlite",
        ]
        for name in old_backup_names:
            success_backup_directory.joinpath(name).write_bytes(b"old backup")
            success_backup_directory.joinpath(f"{name}.json").write_text(
                "{}\n", encoding="utf-8"
            )
        manual_backup = success_backup_directory / "manual.sqlite"
        manual_backup.write_bytes(b"manual backup")
        observations = []
        stop_reader = threading.Event()

        def reader():
            while not stop_reader.is_set():
                try:
                    observations.append(snapshot_count(database))
                except sqlite3.Error as error:
                    observations.append(str(error))

        reader_thread = threading.Thread(target=reader)
        reader_thread.start()
        succeeded = run(
            maintenance_command(tool, binary, database, success_runtime, base)
        )
        stop_reader.set()
        reader_thread.join(timeout=5)
        require(succeeded.returncode == 0, "staged refresh failed", succeeded)
        retained_refresh_backups = sorted(
            success_backup_directory.glob(f"{database.name}.*.sqlite")
        )
        require(
            len(retained_refresh_backups) == 2
            and not success_backup_directory.joinpath(old_backup_names[0]).exists()
            and not success_backup_directory.joinpath(
                f"{old_backup_names[0]}.json"
            ).exists()
            and success_backup_directory.joinpath(old_backup_names[1]).is_file(),
            "successful refresh did not apply default backup retention",
        )
        require(manual_backup.is_file(), "refresh retention removed a manual backup")
        require(snapshot_count(database) == 2, "refreshed snapshot was not published")
        require(
            observations and set(observations).issubset({1, 2}),
            f"readers observed non-atomic database state: {set(observations)}",
        )
        status = json.loads(
            (success_runtime / "last-success.json").read_text(encoding="utf-8")
        )
        require(
            status["state"] == "succeeded"
            and len(status["last_success"]["snapshot_digest"]) == 64
            and status["last_success"]["counts"]["catalog_snapshots"] == 2
            and status["failure"] is None,
            "last-success record is incomplete",
        )

        failed_after_success = run(
            maintenance_command(tool, wrapper, database, success_runtime, base),
            env=environment | {"MCPO_TEST_MODE": "fail"},
        )
        require(failed_after_success.returncode == 47, "later failure was not recorded")
        retained = json.loads(
            (success_runtime / "last-success.json").read_text(encoding="utf-8")
        )
        require(
            retained["state"] == "failed"
            and retained["last_success"]["snapshot_digest"]
            == status["last_success"]["snapshot_digest"],
            "failure status did not retain the last successful publication",
        )

        backup = root / "verified-backup.sqlite"
        backed_up = run(
            [
                sys.executable,
                tool,
                "backup",
                "--database",
                database,
                "--output",
                backup,
            ]
        )
        require(backed_up.returncode == 0 and Path(f"{backup}.json").is_file(), "backup failed", backed_up)

        before_failed_recovery_backup = file_digest(database)
        restore_globals = maintenance["restore_command"].__globals__
        original_publish_backup = restore_globals["publish_backup"]

        def fail_recovery_backup(_source, _destination):
            raise maintenance["MaintenanceError"](
                "backup_failure", "injected pre-restore backup failure"
            )

        restore_globals["publish_backup"] = fail_recovery_backup
        try:
            failed_recovery_backup = maintenance["restore_command"](
                SimpleNamespace(database=database, backup=backup)
            )
        finally:
            restore_globals["publish_backup"] = original_publish_backup
        require(
            failed_recovery_backup != 0,
            "restore ignored a pre-restore backup failure",
        )
        require(
            file_digest(database) == before_failed_recovery_backup,
            "pre-restore backup failure changed the live database",
        )

        valid_live_restore = run(
            [
                sys.executable,
                tool,
                "restore",
                "--database",
                database,
                "--backup",
                backup,
            ]
        )
        require(
            valid_live_restore.returncode == 0,
            "restore with a valid live database failed",
            valid_live_restore,
        )
        valid_live_status = json.loads(valid_live_restore.stdout)
        recovery_status = valid_live_status["pre_restore_backup"]
        require(
            recovery_status["state"] == "created"
            and Path(recovery_status["path"]).is_file()
            and Path(recovery_status["metadata_path"]).is_file(),
            "valid live restore did not create a verified recovery backup",
        )

        with sqlite3.connect(database) as connection:
            connection.execute("DELETE FROM snapshot_server_versions")
            connection.execute("DELETE FROM snapshots")
            connection.commit()
        restored = run(
            [
                sys.executable,
                tool,
                "restore",
                "--database",
                database,
                "--backup",
                backup,
            ]
        )
        require(restored.returncode == 0, "verified restore failed", restored)
        restored_status = json.loads(restored.stdout)
        require(
            restored_status["pre_restore_backup"]["state"] == "skipped"
            and restored_status["pre_restore_backup"]["live_database_validation"][
                "category"
            ]
            == "validation_failure"
            and "warning:" in restored.stderr,
            "invalid live restore did not report the skipped recovery backup",
        )
        require(snapshot_count(database) == 2, "restore did not recover snapshot history")

        before_rejected_restore = file_digest(database)
        with backup.open("r+b") as output:
            output.seek(100)
            output.write(b"tamper")
        rejected = run(
            [
                sys.executable,
                tool,
                "restore",
                "--database",
                database,
                "--backup",
                backup,
            ]
        )
        require(rejected.returncode != 0, "tampered backup was restored", rejected)
        require(
            file_digest(database) == before_rejected_restore,
            "rejected restore changed live database",
        )
    finally:
        server.shutdown()
        server.server_close()
        shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, OSError, sqlite3.Error, subprocess.SubprocessError) as error:
        print(f"maintenance test failure: {error}", file=sys.stderr)
        raise SystemExit(1)
