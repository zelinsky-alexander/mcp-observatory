#!/usr/bin/env python3
"""Bounded, idempotent scheduler for Observatory static artifact analysis."""

from __future__ import annotations

import argparse
from contextlib import contextmanager
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import selectors
import signal
import sqlite3
import subprocess
import sys
import time
from typing import Any, Iterator

ANALYSIS_TYPE = "npm_package_static_v1"
ANALYZER_NAME = "mcp-observatory-static"
ANALYZER_VERSION = "1.1.0"
SUPPORTED_REGISTRIES = {"npm", "pypi"}
MAX_BATCH_SIZE = 1000
MAX_MESSAGE_BYTES = 2048

SCHEMA = """
CREATE TABLE IF NOT EXISTS static_analysis_schedule_profiles(
  profile_key TEXT PRIMARY KEY,
  analysis_type TEXT NOT NULL,
  analyzer_name TEXT NOT NULL,
  analyzer_version TEXT NOT NULL,
  ruleset_version TEXT NOT NULL,
  rules_sha256 TEXT NOT NULL,
  created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
  selected_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
);
CREATE TABLE IF NOT EXISTS static_analysis_schedule_current(
  singleton INTEGER PRIMARY KEY CHECK(singleton=1),
  profile_key TEXT NOT NULL REFERENCES static_analysis_schedule_profiles(profile_key),
  updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
);
CREATE TABLE IF NOT EXISTS static_analysis_schedule_state(
  profile_key TEXT NOT NULL REFERENCES static_analysis_schedule_profiles(profile_key)
    ON DELETE CASCADE,
  package_id INTEGER NOT NULL REFERENCES packages(id) ON DELETE CASCADE,
  state TEXT NOT NULL CHECK(state IN(
    'eligible','running','completed','failed','unsupported','unresolvable'
  )),
  reason_code TEXT,
  reason_message TEXT,
  attempt_count INTEGER NOT NULL DEFAULT 0 CHECK(attempt_count >= 0),
  analysis_run_id INTEGER,
  artifact_sha256 TEXT,
  reused_existing INTEGER NOT NULL DEFAULT 0 CHECK(reused_existing IN(0,1)),
  discovered_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
  last_attempt_at TEXT,
  updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY(profile_key,package_id)
);
CREATE INDEX IF NOT EXISTS static_analysis_schedule_state_lookup
ON static_analysis_schedule_state(profile_key,state,attempt_count,last_attempt_at,package_id);
"""


def canonical(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"))


def bounded_message(value: str) -> str:
    encoded = value.encode("utf-8", "replace")
    if len(encoded) <= MAX_MESSAGE_BYTES:
        return encoded.decode("utf-8", "replace")
    return encoded[:MAX_MESSAGE_BYTES].decode("utf-8", "ignore") + "…"


def table_exists(db: sqlite3.Connection, name: str) -> bool:
    return db.execute(
        "SELECT 1 FROM sqlite_schema WHERE type='table' AND name=?", (name,)
    ).fetchone() is not None


def table_columns(db: sqlite3.Connection, name: str) -> set[str]:
    return {str(row["name"]) for row in db.execute(f"PRAGMA table_info({name})")}


@contextmanager
def writer_lock(database: Path) -> Iterator[None]:
    path = Path(str(database) + ".writer.lock")
    descriptor = os.open(
        path, os.O_RDWR | os.O_CREAT | os.O_CLOEXEC | os.O_NOFOLLOW, 0o600
    )
    try:
        while True:
            try:
                fcntl.flock(descriptor, fcntl.LOCK_EX)
                break
            except InterruptedError:
                continue
        yield
    finally:
        os.close(descriptor)


def connect(database: Path) -> sqlite3.Connection:
    db = sqlite3.connect(database, timeout=30)
    db.row_factory = sqlite3.Row
    db.execute("PRAGMA foreign_keys=ON")
    db.execute("PRAGMA busy_timeout=30000")
    return db


def load_profile(args: argparse.Namespace) -> tuple[dict[str, str], str]:
    rules = Path(args.rules).resolve().read_bytes()
    if len(rules) > 2 * 1024 * 1024:
        raise ValueError("rules file exceeds 2 MiB")
    parsed = json.loads(rules)
    version = parsed.get("ruleset_version") if isinstance(parsed, dict) else None
    if not isinstance(version, str) or not version or len(version) > 128:
        raise ValueError("rules file has no bounded ruleset_version")
    profile = {
        "analysis_type": args.analysis_type,
        "analyzer_name": args.analyzer_name,
        "analyzer_version": args.analyzer_version,
        "ruleset_version": version,
        "rules_sha256": hashlib.sha256(rules).hexdigest(),
    }
    key = hashlib.sha256(canonical(profile).encode("utf-8")).hexdigest()
    return profile, key


def register_profile(db: sqlite3.Connection, profile: dict[str, str], key: str) -> None:
    db.executescript(SCHEMA)
    columns = table_columns(db, "static_analysis_schedule_state")
    if "discovered_at" not in columns:
        db.execute(
            "ALTER TABLE static_analysis_schedule_state ADD COLUMN discovered_at TEXT"
        )
    db.execute(
        """UPDATE static_analysis_schedule_state
           SET discovered_at=CURRENT_TIMESTAMP WHERE discovered_at IS NULL"""
    )
    db.execute(
        """CREATE INDEX IF NOT EXISTS static_analysis_schedule_state_priority
           ON static_analysis_schedule_state(
             profile_key,state,discovered_at,attempt_count,last_attempt_at,package_id
           )"""
    )
    db.execute(
        """INSERT INTO static_analysis_schedule_profiles(
             profile_key,analysis_type,analyzer_name,analyzer_version,
             ruleset_version,rules_sha256)
           VALUES(?,?,?,?,?,?)
           ON CONFLICT(profile_key) DO UPDATE SET selected_at=CURRENT_TIMESTAMP""",
        (
            key,
            profile["analysis_type"],
            profile["analyzer_name"],
            profile["analyzer_version"],
            profile["ruleset_version"],
            profile["rules_sha256"],
        ),
    )
    db.execute(
        """INSERT INTO static_analysis_schedule_current(singleton,profile_key)
           VALUES(1,?)
           ON CONFLICT(singleton) DO UPDATE SET
             profile_key=excluded.profile_key,updated_at=CURRENT_TIMESTAMP""",
        (key,),
    )


def classify(row: sqlite3.Row) -> tuple[str, str | None, str | None]:
    registry = str(row["registry_type"] or "").strip().lower()
    identifier = str(row["identifier"] or "").strip()
    version = str(row["version"] or "").strip()
    if registry not in SUPPORTED_REGISTRIES:
        return (
            "unsupported",
            "unsupported_registry",
            f"registry type {registry or '<empty>'} is not supported",
        )
    if not identifier:
        return "unresolvable", "missing_package_identifier", "package identifier is missing"
    if not version:
        return "unresolvable", "missing_exact_version", "exact package version is missing"
    return "eligible", None, None


def compatible_run(
    db: sqlite3.Connection, package_id: int, profile: dict[str, str]
) -> sqlite3.Row | None:
    if not table_exists(db, "analysis_runs"):
        return None
    return db.execute(
        """SELECT id,artifact_sha256 FROM analysis_runs
           WHERE package_id=? AND status='completed' AND analysis_type=?
             AND analyzer_name=? AND analyzer_version=? AND ruleset_version=?
             AND artifact_sha256 IS NOT NULL
           ORDER BY id DESC LIMIT 1""",
        (
            package_id,
            profile["analysis_type"],
            profile["analyzer_name"],
            profile["analyzer_version"],
            profile["ruleset_version"],
        ),
    ).fetchone()


def scheduled_completed_run(
    db: sqlite3.Connection,
    key: str,
    package_id: int,
    profile: dict[str, str],
) -> sqlite3.Row | None:
    if not table_exists(db, "analysis_runs"):
        return None
    return db.execute(
        """SELECT ar.id,ar.artifact_sha256
           FROM static_analysis_schedule_state s
           JOIN analysis_runs ar ON ar.id=s.analysis_run_id
           WHERE s.profile_key=? AND s.package_id=? AND s.state='completed'
             AND ar.status='completed' AND ar.analysis_type=?
             AND ar.analyzer_name=? AND ar.analyzer_version=?
             AND ar.ruleset_version=? AND ar.artifact_sha256=s.artifact_sha256
           LIMIT 1""",
        (
            key,
            package_id,
            profile["analysis_type"],
            profile["analyzer_name"],
            profile["analyzer_version"],
            profile["ruleset_version"],
        ),
    ).fetchone()


def synchronize(
    db: sqlite3.Connection,
    key: str,
    profile: dict[str, str],
    stale_seconds: int,
) -> None:
    packages = db.execute(
        "SELECT id,registry_type,identifier,version FROM packages ORDER BY id"
    ).fetchall()
    for package in packages:
        package_id = int(package["id"])
        completed = compatible_run(db, package_id, profile)
        if completed is None:
            completed = scheduled_completed_run(db, key, package_id, profile)
        if completed is not None:
            state, code, message = "completed", None, None
            run_id = int(completed["id"])
            artifact = str(completed["artifact_sha256"])
        else:
            state, code, message = classify(package)
            run_id = None
            artifact = None
        existing = db.execute(
            """SELECT state,reason_code,reason_message,analysis_run_id,
                      artifact_sha256
               FROM static_analysis_schedule_state
               WHERE profile_key=? AND package_id=?""",
            (key, package_id),
        ).fetchone()
        if existing is not None and completed is None:
            old = str(existing["state"])
            if old in {
                "failed", "running", "unsupported", "unresolvable"
            } and state == "eligible":
                state = old
                code = str(existing["reason_code"] or "") or None
                message = str(existing["reason_message"] or "") or None
                run_id = existing["analysis_run_id"]
                artifact = existing["artifact_sha256"]
        if existing is None:
            db.execute(
                """INSERT INTO static_analysis_schedule_state(
                     profile_key,package_id,state,reason_code,reason_message,
                     analysis_run_id,artifact_sha256)
                   VALUES(?,?,?,?,?,?,?)""",
                (key, package_id, state, code, message, run_id, artifact),
            )
            continue
        desired = (state, code, message, run_id, artifact)
        current = tuple(existing[name] for name in (
            "state",
            "reason_code",
            "reason_message",
            "analysis_run_id",
            "artifact_sha256",
        ))
        if desired != current:
            db.execute(
                """UPDATE static_analysis_schedule_state
                   SET state=?,reason_code=?,reason_message=?,analysis_run_id=?,
                       artifact_sha256=?,updated_at=CURRENT_TIMESTAMP
                   WHERE profile_key=? AND package_id=?""",
                (*desired, key, package_id),
            )
    db.execute(
        """UPDATE static_analysis_schedule_state
           SET state='failed',reason_code='interrupted',
               reason_message='previous scheduler process did not finish this package',
               updated_at=CURRENT_TIMESTAMP
           WHERE profile_key=? AND state='running'
             AND last_attempt_at <= datetime('now', ?)""",
        (key, f"-{stale_seconds} seconds"),
    )


def candidates(
    db: sqlite3.Connection,
    key: str,
    batch_size: int,
    maximum_attempts: int,
    retry_seconds: int,
) -> list[sqlite3.Row]:
    return db.execute(
        """SELECT s.package_id
           FROM static_analysis_schedule_state s
           WHERE s.profile_key=? AND s.state IN('eligible','failed')
             AND s.attempt_count<?
             AND (s.last_attempt_at IS NULL OR
                  s.last_attempt_at<=datetime('now',?))
           ORDER BY s.discovered_at DESC,
                    CASE s.state WHEN 'eligible' THEN 0 ELSE 1 END,
                    s.attempt_count,s.package_id DESC
           LIMIT ?""",
        (key, maximum_attempts, f"-{retry_seconds} seconds", batch_size),
    ).fetchall()


def run_child(argv: list[str], timeout: int, output_limit: int) -> subprocess.CompletedProcess[bytes]:
    child_environment = {
        "PATH": os.environ.get("PATH", "/usr/bin:/bin"),
        "LANG": "C.UTF-8",
    }
    if os.environ.get("TMPDIR"):
        child_environment["TMPDIR"] = os.environ["TMPDIR"]
    process = subprocess.Popen(
        argv,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        close_fds=True,
        start_new_session=True,
        env=child_environment,
    )
    assert process.stdout is not None and process.stderr is not None
    selector = selectors.DefaultSelector()
    selector.register(process.stdout, selectors.EVENT_READ, "stdout")
    selector.register(process.stderr, selectors.EVENT_READ, "stderr")
    output = {"stdout": bytearray(), "stderr": bytearray()}
    deadline = time.monotonic() + timeout
    failure: str | None = None
    try:
        while selector.get_map() or process.poll() is None:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                failure = f"analysis child exceeded {timeout} seconds"
                break
            for selected, _ in selector.select(timeout=min(0.25, remaining)):
                chunk = os.read(selected.fileobj.fileno(), 8192)
                if not chunk:
                    selector.unregister(selected.fileobj)
                    selected.fileobj.close()
                    continue
                target = output[selected.data]
                if len(target) + len(chunk) > output_limit:
                    failure = "analysis child output exceeded configured limit"
                    break
                target.extend(chunk)
            if failure:
                break
        if failure:
            try:
                os.killpg(process.pid, signal.SIGTERM)
                process.wait(timeout=3)
            except (ProcessLookupError, subprocess.TimeoutExpired):
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                process.wait(timeout=3)
            raise RuntimeError(failure)
        if process.poll() is None:
            process.wait(timeout=3)
    finally:
        selector.close()
        for stream in (process.stdout, process.stderr):
            if not stream.closed:
                stream.close()
    return subprocess.CompletedProcess(
        argv,
        process.returncode,
        bytes(output["stdout"]),
        bytes(output["stderr"]),
    )


def verify_run(
    db: sqlite3.Connection,
    run_id: int,
    artifact: str,
    profile: dict[str, str],
) -> None:
    row = db.execute(
        """SELECT status,analysis_type,analyzer_name,analyzer_version,
                  ruleset_version,artifact_sha256
           FROM analysis_runs WHERE id=?""",
        (run_id,),
    ).fetchone()
    expected = (
        "completed",
        profile["analysis_type"],
        profile["analyzer_name"],
        profile["analyzer_version"],
        profile["ruleset_version"],
        artifact,
    )
    if row is None or tuple(row) != expected:
        raise ValueError("analysis child result does not match the authoritative database run")


def set_running(db: sqlite3.Connection, key: str, package_id: int) -> None:
    db.execute(
        """UPDATE static_analysis_schedule_state
           SET state='running',attempt_count=attempt_count+1,
               last_attempt_at=CURRENT_TIMESTAMP,reason_code=NULL,
               reason_message=NULL,updated_at=CURRENT_TIMESTAMP
           WHERE profile_key=? AND package_id=?""",
        (key, package_id),
    )


def set_result(
    db: sqlite3.Connection,
    key: str,
    package_id: int,
    state: str,
    code: str | None,
    message: str | None,
    run_id: int | None = None,
    artifact: str | None = None,
    reused: bool = False,
) -> None:
    db.execute(
        """UPDATE static_analysis_schedule_state
           SET state=?,reason_code=?,reason_message=?,analysis_run_id=?,
               artifact_sha256=?,reused_existing=?,updated_at=CURRENT_TIMESTAMP
           WHERE profile_key=? AND package_id=?""",
        (
            state,
            code,
            bounded_message(message or "") or None,
            run_id,
            artifact,
            int(reused),
            key,
            package_id,
        ),
    )


def summary(db: sqlite3.Connection, key: str) -> dict[str, Any]:
    row = db.execute(
        """SELECT COUNT(*) total_package_records,
                  SUM(state IN('eligible','running','completed','failed')) eligible_package_records,
                  SUM(state='completed') successfully_analyzed,
                  SUM(state='failed') failed_attempts,
                  SUM(state IN('unsupported','unresolvable')) unsupported_or_unresolvable,
                  SUM(state='eligible' AND attempt_count=0) never_attempted,
                  COUNT(DISTINCT CASE WHEN state='completed'
                    THEN artifact_sha256 END) unique_artifacts_analyzed,
                  SUM(state='running') running
           FROM static_analysis_schedule_state WHERE profile_key=?""",
        (key,),
    ).fetchone()
    result = {name: int(row[name] or 0) for name in row.keys()}
    result["profile_key"] = key
    return result


def remaining_queue(
    db: sqlite3.Connection,
    key: str,
    maximum_attempts: int,
) -> int:
    return int(
        db.execute(
            """SELECT COUNT(*) FROM static_analysis_schedule_state
               WHERE profile_key=? AND state IN('eligible','failed','running')
                 AND attempt_count<?""",
            (key, maximum_attempts),
        ).fetchone()[0]
    )


def claim_next(
    database: Path,
    key: str,
    args: argparse.Namespace,
) -> sqlite3.Row | None:
    with writer_lock(database):
        db = connect(database)
        try:
            selected = candidates(
                db,
                key,
                1,
                args.maximum_attempts,
                args.retry_failed_after_seconds,
            )
            if not selected:
                return None
            item = selected[0]
            with db:
                set_running(db, key, int(item["package_id"]))
            return item
        finally:
            db.close()


def classify_child_failure(returncode: int, stderr: str) -> tuple[str, str]:
    lowered = stderr.lower()
    if "requested url returned error: 404" in lowered:
        return "unresolvable", "registry_not_found"
    if "pypi release metadata identity mismatch" in lowered:
        return "unresolvable", "registry_identity_mismatch"
    if (
        "no supported non-yanked tar-gzip source distribution"
        in lowered
    ):
        return "unsupported", "unsupported_pypi_distribution"
    if (
        "ambiguous package selection" in lowered
        or "exact package record not found" in lowered
    ):
        return "unresolvable", "artifact_unresolvable"
    if returncode == 5:
        if any(token in lowered for token in (
            "unsupported", "wheel", "zip sdist", "yanked"
        )):
            return "unsupported", "artifact_unsupported"
        return "unresolvable", "artifact_unresolvable"
    return "failed", "analysis_failed"


def reclassify_terminal_failures(db: sqlite3.Connection, key: str) -> None:
    rules = (
        ("unresolvable", "registry_not_found",
         "requested url returned error: 404"),
        ("unresolvable", "registry_identity_mismatch",
         "pypi release metadata identity mismatch"),
        ("unsupported", "unsupported_pypi_distribution",
         "no supported non-yanked tar-gzip source distribution"),
        ("unresolvable", "artifact_unresolvable",
         "ambiguous package selection"),
    )
    for state, code, message_fragment in rules:
        db.execute(
            """UPDATE static_analysis_schedule_state
               SET state=?,reason_code=?,updated_at=CURRENT_TIMESTAMP
               WHERE profile_key=? AND state='failed'
                 AND instr(lower(COALESCE(reason_message,'')),?)>0""",
            (state, code, key, message_fragment),
        )


def analyze_claimed(
    database: Path,
    key: str,
    profile: dict[str, str],
    item: sqlite3.Row,
    args: argparse.Namespace,
) -> None:
    package_id = int(item["package_id"])
    argv = [
        str(Path(args.observatory_binary).resolve()),
        "analyze",
        "package",
        "--database",
        str(database),
        "--package-id",
        str(package_id),
        "--evidence-root",
        str(Path(args.evidence_root).resolve()),
        "--rules",
        str(Path(args.rules).resolve()),
        "--format",
        "json",
    ]
    if args.npm_registry_url:
        argv += ["--npm-registry-url", args.npm_registry_url]
    if args.pypi_registry_url:
        argv += ["--pypi-registry-url", args.pypi_registry_url]

    state, code, message = "failed", "analysis_failed", ""
    run_id: int | None = None
    artifact: str | None = None
    reused = False
    try:
        child = run_child(
            argv, args.child_timeout_seconds, args.maximum_child_output_bytes
        )
        stderr = child.stderr.decode("utf-8", "replace")
        if child.returncode == 0:
            payload = json.loads(child.stdout)
            if payload.get("status") != "completed":
                raise ValueError("analysis child did not report completed status")
            run_id = int(payload["analysis_run_id"])
            artifact = str(payload["artifact_sha256"])
            if re.fullmatch(r"[0-9a-f]{64}", artifact) is None:
                raise ValueError("analysis child returned invalid artifact digest")
            reused = bool(payload.get("reused_existing", False))
            state, code, message = "completed", None, None
        else:
            state, code = classify_child_failure(child.returncode, stderr)
            message = stderr or f"analysis child exited with status {child.returncode}"
    except (OSError, RuntimeError, ValueError, KeyError, json.JSONDecodeError) as exc:
        message = str(exc)

    with writer_lock(database):
        db = connect(database)
        try:
            try:
                with db:
                    if state == "completed":
                        assert run_id is not None and artifact is not None
                        verify_run(db, run_id, artifact, profile)
                    set_result(
                        db,
                        key,
                        package_id,
                        state,
                        code,
                        message,
                        run_id,
                        artifact,
                        reused,
                    )
            except (ValueError, sqlite3.Error) as exc:
                with db:
                    set_result(
                        db,
                        key,
                        package_id,
                        "failed",
                        "result_verification_failed",
                        str(exc),
                    )
        finally:
            db.close()


def execute(args: argparse.Namespace) -> dict[str, Any]:
    started = time.monotonic()
    deadline = (
        started + args.maximum_run_seconds
        if args.maximum_run_seconds > 0
        else None
    )
    database = Path(args.database).resolve()
    profile, key = load_profile(args)
    with writer_lock(database):
        db = connect(database)
        try:
            with db:
                register_profile(db, profile, key)
                reclassify_terminal_failures(db, key)
                synchronize(db, key, profile, args.stale_running_after_seconds)
        finally:
            db.close()

    processed = 0
    stop_reason = "batch_limit"
    while processed < args.batch_size:
        if deadline is not None:
            remaining = deadline - time.monotonic()
            if remaining <= args.child_timeout_seconds:
                stop_reason = "time_budget"
                break
        item = claim_next(database, key, args)
        if item is None:
            stop_reason = "queue_empty"
            break
        analyze_claimed(database, key, profile, item, args)
        processed += 1

    elapsed = max(0.0, time.monotonic() - started)
    with writer_lock(database):
        db = connect(database)
        try:
            result = summary(db, key)
            queued = remaining_queue(db, key, args.maximum_attempts)
        finally:
            db.close()
    result["processed_in_batch"] = processed
    result["remaining_queue_records"] = queued
    result["run_elapsed_seconds"] = round(elapsed, 3)
    result["stop_reason"] = stop_reason
    if processed > 0:
        mean = elapsed / processed
        result["mean_seconds_per_processed_record"] = round(mean, 3)
        result["estimated_remaining_seconds"] = round(mean * queued)
    else:
        result["mean_seconds_per_processed_record"] = None
        result["estimated_remaining_seconds"] = None
    return result


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--database", required=True)
    parser.add_argument("--observatory-binary", required=True)
    parser.add_argument("--rules", default="rules/artifact-static-analysis-v1.json")
    parser.add_argument("--evidence-root", default="evidence")
    parser.add_argument("--batch-size", type=int, default=25)
    parser.add_argument("--maximum-run-seconds", type=int, default=0)
    parser.add_argument("--maximum-attempts", type=int, default=3)
    parser.add_argument("--retry-failed-after-seconds", type=int, default=86400)
    parser.add_argument("--stale-running-after-seconds", type=int, default=3600)
    parser.add_argument("--child-timeout-seconds", type=int, default=300)
    parser.add_argument("--maximum-child-output-bytes", type=int, default=1024 * 1024)
    parser.add_argument("--analysis-type", default=ANALYSIS_TYPE)
    parser.add_argument("--analyzer-name", default=ANALYZER_NAME)
    parser.add_argument("--analyzer-version", default=ANALYZER_VERSION)
    parser.add_argument("--npm-registry-url")
    parser.add_argument("--pypi-registry-url")
    parser.add_argument("--format", choices=("text", "json"), default="text")
    args = parser.parse_args()
    if not 1 <= args.batch_size <= MAX_BATCH_SIZE:
        parser.error(f"--batch-size must be between 1 and {MAX_BATCH_SIZE}")
    for name in (
        "maximum_attempts",
        "child_timeout_seconds",
        "maximum_child_output_bytes",
    ):
        if getattr(args, name) <= 0:
            parser.error(f"--{name.replace('_', '-')} must be positive")
    if args.maximum_run_seconds < 0:
        parser.error("--maximum-run-seconds must be non-negative")
    if (
        args.maximum_run_seconds > 0
        and args.maximum_run_seconds <= args.child_timeout_seconds
    ):
        parser.error(
            "--maximum-run-seconds must exceed --child-timeout-seconds"
        )
    if args.retry_failed_after_seconds < 0 or args.stale_running_after_seconds < 0:
        parser.error("retry and stale durations must be non-negative")
    return args


def main() -> int:
    args = parse_args()
    result = execute(args)
    if args.format == "json":
        print(canonical(result))
    else:
        for key, value in result.items():
            print(f"{key}={value}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, sqlite3.Error, ValueError, json.JSONDecodeError) as exc:
        print(f"bulk static analysis failed: {exc}", file=sys.stderr)
        raise SystemExit(2)
