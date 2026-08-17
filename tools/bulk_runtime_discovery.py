#!/usr/bin/env python3
"""Bounded, idempotent scheduler for Native Guard runtime discovery.

Milestone 1 deliberately remains discovery-only: every eligible exact npm stdio
package is run through the existing restricted ``runtime_discovery.py`` pipeline.
The scheduler records explicit terminal states, binds work to a Guard/probe/runtime
profile, and automatically compares a new completed observation with the previous
compatible server version produced by the same scheduler profile.
"""

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

MAX_BATCH_SIZE = 250
MAX_MESSAGE_BYTES = 2048
SCHEDULER_VERSION = "1.0.0"

SCHEMA = """
CREATE TABLE IF NOT EXISTS runtime_discovery_schedule_profiles(
  profile_key TEXT PRIMARY KEY,
  scheduler_version TEXT NOT NULL,
  guard_sha256 TEXT NOT NULL,
  runtime_image TEXT NOT NULL,
  probe_profile_sha256 TEXT NOT NULL,
  runner_sha256 TEXT NOT NULL,
  created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
  selected_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
);
CREATE TABLE IF NOT EXISTS runtime_discovery_schedule_current(
  singleton INTEGER PRIMARY KEY CHECK(singleton=1),
  profile_key TEXT NOT NULL REFERENCES runtime_discovery_schedule_profiles(profile_key),
  updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
);
CREATE TABLE IF NOT EXISTS runtime_discovery_schedule_state(
  profile_key TEXT NOT NULL REFERENCES runtime_discovery_schedule_profiles(profile_key)
    ON DELETE CASCADE,
  package_id INTEGER NOT NULL REFERENCES packages(id) ON DELETE CASCADE,
  state TEXT NOT NULL CHECK(state IN(
    'eligible','running','completed','failed','unsupported','unresolvable'
  )),
  reason_code TEXT,
  reason_message TEXT,
  attempt_count INTEGER NOT NULL DEFAULT 0 CHECK(attempt_count >= 0),
  runtime_observation_run_id INTEGER,
  artifact_sha256 TEXT,
  launch_profile_sha256 TEXT,
  previous_compatible_run_id INTEGER,
  added_tools INTEGER,
  removed_tools INTEGER,
  modified_tools INTEGER,
  unchanged_tools INTEGER,
  discovered_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
  last_attempt_at TEXT,
  updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY(profile_key,package_id)
);
CREATE INDEX IF NOT EXISTS runtime_discovery_schedule_state_lookup
ON runtime_discovery_schedule_state(
  profile_key,state,discovered_at,attempt_count,last_attempt_at,package_id
);
"""


def canonical(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"))


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def bounded_message(value: str) -> str:
    encoded = value.encode("utf-8", "replace")
    if len(encoded) <= MAX_MESSAGE_BYTES:
        return encoded.decode("utf-8", "replace")
    return encoded[:MAX_MESSAGE_BYTES].decode("utf-8", "ignore") + "…"


def table_exists(db: sqlite3.Connection, name: str) -> bool:
    return db.execute(
        "SELECT 1 FROM sqlite_schema WHERE type='table' AND name=?", (name,)
    ).fetchone() is not None


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


@contextmanager
def scheduler_lock(database: Path) -> Iterator[None]:
    """Allow only one bulk runtime scheduler per authoritative catalog."""
    path = Path(str(database) + ".runtime-discovery.lock")
    descriptor = os.open(
        path, os.O_RDWR | os.O_CREAT | os.O_CLOEXEC | os.O_NOFOLLOW, 0o600
    )
    try:
        try:
            fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as exc:
            raise RuntimeError("another runtime-discovery scheduler is already running") from exc
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
    guard = Path(args.guard_binary).resolve()
    runner = Path(args.runtime_runner).resolve()
    probe = Path(args.probe_profile).resolve()
    for label, path in (("guard binary", guard), ("runtime runner", runner), ("probe profile", probe)):
        if not path.is_file():
            raise ValueError(f"{label} does not exist: {path}")
    if probe.stat().st_size > 64 * 1024:
        raise ValueError("probe profile exceeds 64 KiB")
    probe_bytes = probe.read_bytes()
    parsed = json.loads(probe_bytes)
    if not isinstance(parsed, dict):
        raise ValueError("probe profile must be a JSON object")
    required = {
        "profile_version": 1,
        "transport": "stdio",
        "guard_command": "inspect",
        "tool_invocation": False,
        "inventory_version": 1,
    }
    for name, expected in required.items():
        if parsed.get(name) != expected:
            raise ValueError(f"probe profile {name} must be {expected!r}")
    profile_id = parsed.get("profile_id")
    if not isinstance(profile_id, str) or not profile_id or len(profile_id) > 128:
        raise ValueError("probe profile has no bounded profile_id")
    profile = {
        "scheduler_version": SCHEDULER_VERSION,
        "guard_sha256": sha256_file(guard),
        "runtime_image": str(args.runtime_image),
        "probe_profile_sha256": hashlib.sha256(probe_bytes).hexdigest(),
        "runner_sha256": sha256_file(runner),
    }
    key = hashlib.sha256(canonical(profile).encode("utf-8")).hexdigest()
    return profile, key


def register_profile(db: sqlite3.Connection, profile: dict[str, str], key: str) -> None:
    db.executescript(SCHEMA)
    db.execute(
        """INSERT INTO runtime_discovery_schedule_profiles(
             profile_key,scheduler_version,guard_sha256,runtime_image,
             probe_profile_sha256,runner_sha256)
           VALUES(?,?,?,?,?,?)
           ON CONFLICT(profile_key) DO UPDATE SET selected_at=CURRENT_TIMESTAMP""",
        (
            key,
            profile["scheduler_version"],
            profile["guard_sha256"],
            profile["runtime_image"],
            profile["probe_profile_sha256"],
            profile["runner_sha256"],
        ),
    )
    db.execute(
        """INSERT INTO runtime_discovery_schedule_current(singleton,profile_key)
           VALUES(1,?)
           ON CONFLICT(singleton) DO UPDATE SET
             profile_key=excluded.profile_key,updated_at=CURRENT_TIMESTAMP""",
        (key,),
    )


def classify(row: sqlite3.Row) -> tuple[str, str | None, str | None]:
    registry = str(row["registry_type"] or "").strip().lower()
    transport = str(row["transport"] or "").strip().lower()
    identifier = str(row["identifier"] or "").strip()
    version = str(row["version"] or "").strip()
    if registry != "npm":
        return "unsupported", "unsupported_ecosystem", f"registry type {registry or '<empty>'} is not npm"
    if transport != "stdio":
        return "unsupported", "unsupported_transport", f"transport {transport or '<empty>'} is not stdio"
    if not identifier:
        return "unresolvable", "missing_package_identifier", "package identifier is missing"
    if not version:
        return "unresolvable", "missing_exact_version", "exact npm package version is missing"
    return "eligible", None, None


def _completed_state_is_valid(
    db: sqlite3.Connection,
    run_id: int | None,
    package_id: int,
    profile: dict[str, str],
) -> bool:
    if run_id is None or not table_exists(db, "runtime_observation_runs"):
        return False
    row = db.execute(
        """SELECT status,package_id,guard_version,sandbox_image,artifact_sha256,
                  launch_profile_sha256
           FROM runtime_observation_runs WHERE id=?""",
        (run_id,),
    ).fetchone()
    if row is None:
        return False
    return (
        row["status"] == "completed"
        and int(row["package_id"]) == package_id
        and row["guard_version"] == "sha256:" + profile["guard_sha256"]
        and row["sandbox_image"] == profile["runtime_image"]
        and isinstance(row["artifact_sha256"], str)
        and isinstance(row["launch_profile_sha256"], str)
    )


def synchronize(
    db: sqlite3.Connection,
    key: str,
    profile: dict[str, str],
    stale_seconds: int,
) -> None:
    packages = db.execute(
        "SELECT id,registry_type,identifier,version,transport FROM packages ORDER BY id"
    ).fetchall()
    for package in packages:
        package_id = int(package["id"])
        existing = db.execute(
            """SELECT state,reason_code,reason_message,runtime_observation_run_id,
                      artifact_sha256,launch_profile_sha256,previous_compatible_run_id,
                      added_tools,removed_tools,modified_tools,unchanged_tools
               FROM runtime_discovery_schedule_state
               WHERE profile_key=? AND package_id=?""",
            (key, package_id),
        ).fetchone()
        base_state, base_code, base_message = classify(package)
        if existing is None:
            db.execute(
                """INSERT INTO runtime_discovery_schedule_state(
                     profile_key,package_id,state,reason_code,reason_message)
                   VALUES(?,?,?,?,?)""",
                (key, package_id, base_state, base_code, base_message),
            )
            continue

        old_state = str(existing["state"])
        if old_state == "completed":
            valid = _completed_state_is_valid(
                db,
                int(existing["runtime_observation_run_id"])
                if existing["runtime_observation_run_id"] is not None
                else None,
                package_id,
                profile,
            )
            if valid:
                continue
            db.execute(
                """UPDATE runtime_discovery_schedule_state
                   SET state=?,reason_code=?,reason_message=?,
                       runtime_observation_run_id=NULL,artifact_sha256=NULL,
                       launch_profile_sha256=NULL,previous_compatible_run_id=NULL,
                       added_tools=NULL,removed_tools=NULL,modified_tools=NULL,
                       unchanged_tools=NULL,updated_at=CURRENT_TIMESTAMP
                   WHERE profile_key=? AND package_id=?""",
                (base_state, base_code, base_message, key, package_id),
            )
            continue

        if old_state in {"failed", "running"} and base_state == "eligible":
            continue
        if old_state != base_state or existing["reason_code"] != base_code:
            db.execute(
                """UPDATE runtime_discovery_schedule_state
                   SET state=?,reason_code=?,reason_message=?,updated_at=CURRENT_TIMESTAMP
                   WHERE profile_key=? AND package_id=?""",
                (base_state, base_code, base_message, key, package_id),
            )

    db.execute(
        """UPDATE runtime_discovery_schedule_state
           SET state='failed',reason_code='interrupted',
               reason_message='previous runtime scheduler did not finish this package',
               updated_at=CURRENT_TIMESTAMP
           WHERE profile_key=? AND state='running'
             AND last_attempt_at <= datetime('now', ?)""",
        (key, f"-{stale_seconds} seconds"),
    )


def candidates(
    db: sqlite3.Connection,
    key: str,
    limit: int,
    maximum_attempts: int,
    retry_seconds: int,
) -> list[sqlite3.Row]:
    return db.execute(
        """SELECT s.package_id,sv.id AS server_version_id,
                  sv.server_identifier,sv.server_version,
                  p.identifier AS package_identifier
           FROM runtime_discovery_schedule_state s
           JOIN packages p ON p.id=s.package_id
           JOIN server_versions sv ON sv.id=p.server_version_id
           WHERE s.profile_key=? AND s.state IN('eligible','failed')
             AND s.attempt_count<?
             AND (s.last_attempt_at IS NULL OR
                  s.last_attempt_at<=datetime('now',?))
           ORDER BY s.discovered_at DESC,
                    CASE s.state WHEN 'eligible' THEN 0 ELSE 1 END,
                    s.attempt_count,s.package_id DESC
           LIMIT ?""",
        (key, maximum_attempts, f"-{retry_seconds} seconds", limit),
    ).fetchall()


def set_running(db: sqlite3.Connection, key: str, package_id: int) -> None:
    db.execute(
        """UPDATE runtime_discovery_schedule_state
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
    *,
    run_id: int | None = None,
    artifact: str | None = None,
    launch_profile: str | None = None,
    previous_run_id: int | None = None,
    drift: dict[str, list[str]] | None = None,
) -> None:
    counts = {
        "added": None,
        "removed": None,
        "modified": None,
        "unchanged": None,
    }
    if drift is not None:
        counts = {name: len(drift[name]) for name in counts}
    db.execute(
        """UPDATE runtime_discovery_schedule_state
           SET state=?,reason_code=?,reason_message=?,runtime_observation_run_id=?,
               artifact_sha256=?,launch_profile_sha256=?,previous_compatible_run_id=?,
               added_tools=?,removed_tools=?,modified_tools=?,unchanged_tools=?,
               updated_at=CURRENT_TIMESTAMP
           WHERE profile_key=? AND package_id=?""",
        (
            state,
            code,
            bounded_message(message or "") or None,
            run_id,
            artifact,
            launch_profile,
            previous_run_id,
            counts["added"],
            counts["removed"],
            counts["modified"],
            counts["unchanged"],
            key,
            package_id,
        ),
    )


def claim_next(database: Path, key: str, args: argparse.Namespace) -> sqlite3.Row | None:
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


def run_child(argv: list[str], timeout: int, output_limit: int) -> subprocess.CompletedProcess[bytes]:
    process = subprocess.Popen(
        argv,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        close_fds=True,
        start_new_session=True,
        env={"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "LANG": "C.UTF-8"},
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
                failure = f"runtime discovery child exceeded {timeout} seconds"
                break
            for selected, _ in selector.select(timeout=min(0.25, remaining)):
                chunk = os.read(selected.fileobj.fileno(), 8192)
                if not chunk:
                    selector.unregister(selected.fileobj)
                    selected.fileobj.close()
                    continue
                target = output[selected.data]
                if len(target) + len(chunk) > output_limit:
                    failure = "runtime discovery child output exceeded configured limit"
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
    package_id: int,
    artifact: str,
    launch_profile: str,
    profile: dict[str, str],
) -> None:
    row = db.execute(
        """SELECT status,package_id,artifact_sha256,launch_profile_sha256,
                  sandbox_image,guard_version
           FROM runtime_observation_runs WHERE id=?""",
        (run_id,),
    ).fetchone()
    expected = (
        "completed",
        package_id,
        artifact,
        launch_profile,
        profile["runtime_image"],
        "sha256:" + profile["guard_sha256"],
    )
    if row is None or tuple(row) != expected:
        raise ValueError("runtime child result does not match the authoritative observation row")


def previous_compatible_run(
    db: sqlite3.Connection,
    key: str,
    current_run_id: int,
    current_server_version_id: int,
    server_identifier: str,
    package_identifier: str,
) -> int | None:
    """Return a prior different server version produced by this exact profile."""
    if not table_exists(db, "runtime_observation_runs"):
        return None
    row = db.execute(
        """SELECT r.id
           FROM runtime_discovery_schedule_state s
           JOIN runtime_observation_runs r ON r.id=s.runtime_observation_run_id
           JOIN server_versions sv ON sv.id=r.server_version_id
           JOIN packages p ON p.id=r.package_id
           WHERE s.profile_key=? AND s.state='completed'
             AND r.status='completed' AND r.id<>?
             AND r.server_version_id<>?
             AND sv.server_identifier=? AND p.identifier=?
           ORDER BY COALESCE(r.completed_at,r.started_at,'') COLLATE BINARY DESC,
                    r.id DESC
           LIMIT 1""",
        (
            key,
            current_run_id,
            current_server_version_id,
            server_identifier,
            package_identifier,
        ),
    ).fetchone()
    return None if row is None else int(row["id"])


def compare_runs(db: sqlite3.Connection, older: int, newer: int) -> dict[str, list[str]]:
    def load(run_id: int) -> dict[str, str]:
        return {
            str(row["name"]): str(row["definition_json"])
            for row in db.execute(
                "SELECT name,definition_json FROM runtime_observation_tools WHERE run_id=?",
                (run_id,),
            )
        }

    left, right = load(older), load(newer)
    return {
        "added": sorted(right.keys() - left.keys()),
        "removed": sorted(left.keys() - right.keys()),
        "modified": sorted(
            name for name in left.keys() & right.keys() if left[name] != right[name]
        ),
        "unchanged": sorted(
            name for name in left.keys() & right.keys() if left[name] == right[name]
        ),
    }


def classify_child_failure(stderr: str, returncode: int) -> tuple[str, str, str]:
    lowered = stderr.lower()
    if "no unambiguous executable bin entry" in lowered:
        return "unsupported", "ambiguous_executable", stderr
    if "offline install failed" in lowered:
        return "failed", "install_failed", stderr
    if "mcp-native-guard inspect failed" in lowered:
        return "failed", "protocol_failed", stderr
    if any(token in lowered for token in (
        "npm metadata identity mismatch",
        "npm metadata has no dist object",
        "npm metadata lacks tarball",
        "npm integrity mismatch",
        "npm artifact exceeded limit",
    )):
        return "unresolvable", "artifact_unresolvable", stderr
    return "failed", "runtime_discovery_failed", stderr or f"runtime discovery exited with status {returncode}"


def discover_claimed(
    database: Path,
    key: str,
    profile: dict[str, str],
    item: sqlite3.Row,
    args: argparse.Namespace,
) -> None:
    package_id = int(item["package_id"])
    argv = [
        sys.executable,
        str(Path(args.runtime_runner).resolve()),
        "observe",
        "--database",
        str(database),
        "--server",
        str(item["server_identifier"]),
        "--version",
        str(item["server_version"]),
        "--package",
        str(item["package_identifier"]),
        "--guard-binary",
        str(Path(args.guard_binary).resolve()),
        "--evidence-root",
        str(Path(args.evidence_root).resolve()),
        "--runtime-image",
        str(args.runtime_image),
        "--timeout",
        str(args.phase_timeout_seconds),
    ]

    state, code, message = "failed", "runtime_discovery_failed", ""
    run_id: int | None = None
    artifact: str | None = None
    launch_profile: str | None = None
    previous_run_id: int | None = None
    drift: dict[str, list[str]] | None = None
    try:
        child = run_child(argv, args.child_timeout_seconds, args.maximum_child_output_bytes)
        stderr = child.stderr.decode("utf-8", "replace")
        if child.returncode != 0:
            state, code, message = classify_child_failure(stderr, child.returncode)
        else:
            payload = json.loads(child.stdout)
            if payload.get("status") != "completed":
                raise ValueError("runtime child did not report completed status")
            run_id = int(payload["runtime_observation_run_id"])
            artifact = str(payload["artifact_sha256"])
            launch_profile = str(payload["launch_profile_sha256"])
            if re.fullmatch(r"[0-9a-f]{64}", artifact) is None:
                raise ValueError("runtime child returned invalid artifact digest")
            if re.fullmatch(r"[0-9a-f]{64}", launch_profile) is None:
                raise ValueError("runtime child returned invalid launch-profile digest")
            state, code, message = "completed", None, None
    except (OSError, RuntimeError, ValueError, KeyError, json.JSONDecodeError) as exc:
        message = str(exc)

    with writer_lock(database):
        db = connect(database)
        try:
            try:
                with db:
                    if state == "completed":
                        assert run_id is not None and artifact is not None and launch_profile is not None
                        verify_run(db, run_id, package_id, artifact, launch_profile, profile)
                        previous_run_id = previous_compatible_run(
                            db,
                            key,
                            run_id,
                            int(item["server_version_id"]),
                            str(item["server_identifier"]),
                            str(item["package_identifier"]),
                        )
                        if previous_run_id is not None:
                            drift = compare_runs(db, previous_run_id, run_id)
                    set_result(
                        db,
                        key,
                        package_id,
                        state,
                        code,
                        message,
                        run_id=run_id,
                        artifact=artifact,
                        launch_profile=launch_profile,
                        previous_run_id=previous_run_id,
                        drift=drift,
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


def summary(db: sqlite3.Connection, key: str) -> dict[str, Any]:
    row = db.execute(
        """SELECT COUNT(*) total_package_records,
                  SUM(state IN('eligible','running','completed','failed')) eligible_package_records,
                  SUM(state='completed') completed_observations,
                  SUM(state='failed') failed_attempts,
                  SUM(state IN('unsupported','unresolvable')) unsupported_or_unresolvable,
                  SUM(state='eligible' AND attempt_count=0) never_attempted,
                  COUNT(DISTINCT CASE WHEN state='completed' THEN artifact_sha256 END)
                    unique_artifacts_observed,
                  SUM(state='running') running,
                  SUM(state='completed' AND previous_compatible_run_id IS NOT NULL)
                    comparable_observations,
                  SUM(state='completed' AND previous_compatible_run_id IS NOT NULL
                      AND (COALESCE(added_tools,0)+COALESCE(removed_tools,0)+
                           COALESCE(modified_tools,0))>0) drifted_observations
           FROM runtime_discovery_schedule_state WHERE profile_key=?""",
        (key,),
    ).fetchone()
    result = {name: int(row[name] or 0) for name in row.keys()}
    result["profile_key"] = key
    return result


def remaining_queue(db: sqlite3.Connection, key: str, maximum_attempts: int) -> int:
    return int(
        db.execute(
            """SELECT COUNT(*) FROM runtime_discovery_schedule_state
               WHERE profile_key=? AND state IN('eligible','failed','running')
                 AND attempt_count<?""",
            (key, maximum_attempts),
        ).fetchone()[0]
    )


def execute(args: argparse.Namespace) -> dict[str, Any]:
    started = time.monotonic()
    deadline = started + args.maximum_run_seconds if args.maximum_run_seconds > 0 else None
    database = Path(args.database).resolve()
    profile, key = load_profile(args)

    with scheduler_lock(database):
        with writer_lock(database):
            db = connect(database)
            try:
                with db:
                    register_profile(db, profile, key)
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
            discover_claimed(database, key, profile, item, args)
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
    result["probe_profile_sha256"] = profile["probe_profile_sha256"]
    result["guard_sha256"] = profile["guard_sha256"]
    result["runtime_image"] = profile["runtime_image"]
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
    parser.add_argument("--runtime-runner", required=True)
    parser.add_argument("--guard-binary", required=True)
    parser.add_argument("--probe-profile", required=True)
    parser.add_argument("--evidence-root", required=True)
    parser.add_argument("--runtime-image", default="node:22-bookworm-slim")
    parser.add_argument("--batch-size", type=int, default=10)
    parser.add_argument("--maximum-run-seconds", type=int, default=3000)
    parser.add_argument("--maximum-attempts", type=int, default=3)
    parser.add_argument("--retry-failed-after-seconds", type=int, default=86400)
    parser.add_argument("--stale-running-after-seconds", type=int, default=7200)
    parser.add_argument("--phase-timeout-seconds", type=int, default=180)
    parser.add_argument("--child-timeout-seconds", type=int, default=720)
    parser.add_argument("--maximum-child-output-bytes", type=int, default=1024 * 1024)
    parser.add_argument("--format", choices=("text", "json"), default="text")
    args = parser.parse_args()
    if not 1 <= args.batch_size <= MAX_BATCH_SIZE:
        parser.error(f"--batch-size must be between 1 and {MAX_BATCH_SIZE}")
    for name in (
        "maximum_attempts",
        "phase_timeout_seconds",
        "child_timeout_seconds",
        "maximum_child_output_bytes",
    ):
        if getattr(args, name) <= 0:
            parser.error(f"--{name.replace('_', '-')} must be positive")
    if args.maximum_run_seconds < 0:
        parser.error("--maximum-run-seconds must be non-negative")
    if args.maximum_run_seconds > 0 and args.maximum_run_seconds <= args.child_timeout_seconds:
        parser.error("--maximum-run-seconds must exceed --child-timeout-seconds")
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
    except (OSError, sqlite3.Error, ValueError, RuntimeError, json.JSONDecodeError) as exc:
        print(f"bulk runtime discovery failed: {exc}", file=sys.stderr)
        raise SystemExit(2)
