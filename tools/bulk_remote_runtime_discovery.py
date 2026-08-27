#!/usr/bin/env python3
"""Bounded scheduler for registry-declared remote MCP endpoint observations.

The scheduler runs in the same runtime-discovery service batch as local package
observation. It probes only exact HTTP(S) URLs already present in ``remotes``;
there is no URL/path guessing, port scanning, or tool invocation.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import time
from typing import Any

SCHEDULER_VERSION = "1.2.0"
SUPPORTED_TRANSPORTS = {"streamable-http", "streamable_http", "http", "https"}

SCHEMA = """
CREATE TABLE IF NOT EXISTS runtime_remote_schedule_profiles(
  profile_key TEXT PRIMARY KEY,
  scheduler_version TEXT NOT NULL,
  probe_profile_sha256 TEXT NOT NULL,
  runner_sha256 TEXT NOT NULL,
  created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
  selected_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
);
CREATE TABLE IF NOT EXISTS runtime_remote_schedule_current(
  singleton INTEGER PRIMARY KEY CHECK(singleton=1),
  profile_key TEXT NOT NULL REFERENCES runtime_remote_schedule_profiles(profile_key),
  updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
);
CREATE TABLE IF NOT EXISTS runtime_remote_schedule_state(
  profile_key TEXT NOT NULL REFERENCES runtime_remote_schedule_profiles(profile_key) ON DELETE CASCADE,
  remote_id INTEGER NOT NULL REFERENCES remotes(id) ON DELETE CASCADE,
  state TEXT NOT NULL CHECK(state IN(
    'eligible','running','completed','failed','blocked','inconclusive','unsupported','unresolvable'
  )),
  reason_code TEXT,
  reason_message TEXT,
  attempt_count INTEGER NOT NULL DEFAULT 0,
  runtime_remote_observation_run_id INTEGER,
  inventory_sha256 TEXT,
  previous_compatible_run_id INTEGER,
  added_tools INTEGER,
  removed_tools INTEGER,
  modified_tools INTEGER,
  unchanged_tools INTEGER,
  discovered_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
  last_attempt_at TEXT,
  updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY(profile_key,remote_id)
);
CREATE INDEX IF NOT EXISTS runtime_remote_schedule_state_lookup
ON runtime_remote_schedule_state(profile_key,state,attempt_count,last_attempt_at,remote_id);
"""


def canonical(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"))


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def connect(path: Path) -> sqlite3.Connection:
    db = sqlite3.connect(path, timeout=30)
    db.row_factory = sqlite3.Row
    db.execute("PRAGMA foreign_keys=ON")
    db.execute("PRAGMA busy_timeout=30000")
    db.executescript(SCHEMA)
    return db


def classify(row: sqlite3.Row) -> tuple[str, str | None, str | None]:
    url = str(row["url"] or "").strip()
    transport = str(row["transport"] or "").strip().lower()
    if not url:
        return "unresolvable", "missing_remote_url", "registry remote URL is empty"
    if not (url.startswith("https://") or url.startswith("http://")):
        return "unsupported", "unsupported_remote_scheme", "declared remote is not HTTP(S)"
    if transport not in SUPPORTED_TRANSPORTS:
        return (
            "unsupported",
            "unsupported_remote_transport",
            f"transport {transport or '<empty>'} is not Streamable HTTP",
        )
    return "eligible", None, None


def load_profile(args: argparse.Namespace) -> tuple[dict[str, str], str]:
    runner = Path(args.remote_runner).resolve()
    profile = Path(args.probe_profile).resolve()
    if not runner.is_file() or not profile.is_file():
        raise ValueError("remote runner and probe profile must exist")
    profile_bytes = profile.read_bytes()
    parsed = json.loads(profile_bytes)
    required = {
        "profile_version": 1,
        "transport": "streamable-http",
        "tool_invocation": False,
        "inventory_version": 1,
    }
    for name, expected in required.items():
        if parsed.get(name) != expected:
            raise ValueError(f"remote probe profile {name} must be {expected!r}")
    identity = {
        "scheduler_version": SCHEDULER_VERSION,
        "probe_profile_sha256": hashlib.sha256(profile_bytes).hexdigest(),
        "runner_sha256": sha256_file(runner),
    }
    key = hashlib.sha256(canonical(identity).encode("utf-8")).hexdigest()
    return identity, key


def recover_stale_running(
    db: sqlite3.Connection, key: str, stale_seconds: int
) -> None:
    age = f"-{stale_seconds} seconds"

    # Observation rows can outlive the scheduler process even if the schedule row
    # has already been changed by a child-timeout path. Recover them independently.
    db.execute(
        """UPDATE runtime_remote_observation_runs
           SET status='inconclusive',completed_at=CURRENT_TIMESTAMP,
               error_stage='interrupted',
               error_message='remote runtime scheduler was interrupted before completion'
           WHERE status='running' AND started_at<=datetime('now',?)""",
        (age,),
    )

    stale = db.execute(
        """SELECT remote_id FROM runtime_remote_schedule_state
           WHERE profile_key=? AND state='running'
             AND last_attempt_at<=datetime('now',?)""",
        (key, age),
    ).fetchall()
    for row in stale:
        db.execute(
            """UPDATE runtime_remote_schedule_state
               SET state='inconclusive',reason_code='remote_interrupted',
                   reason_message='previous remote runtime scheduler did not finish this endpoint',
                   updated_at=CURRENT_TIMESTAMP
               WHERE profile_key=? AND remote_id=? AND state='running'""",
            (key, int(row["remote_id"])),
        )


def synchronize(
    db: sqlite3.Connection,
    key: str,
    reprobe_seconds: int,
    stale_running_seconds: int,
) -> None:
    recover_stale_running(db, key, stale_running_seconds)
    rows = db.execute("SELECT id,url,transport FROM remotes ORDER BY id").fetchall()
    for row in rows:
        state, code, message = classify(row)
        existing = db.execute(
            """SELECT state,last_attempt_at FROM runtime_remote_schedule_state
               WHERE profile_key=? AND remote_id=?""",
            (key, int(row["id"])),
        ).fetchone()
        if existing is None:
            db.execute(
                """INSERT INTO runtime_remote_schedule_state(
                     profile_key,remote_id,state,reason_code,reason_message) VALUES(?,?,?,?,?)""",
                (key, int(row["id"]), state, code, message),
            )
            continue
        if state != "eligible":
            if existing["state"] != state:
                db.execute(
                    """UPDATE runtime_remote_schedule_state
                       SET state=?,reason_code=?,reason_message=?,updated_at=CURRENT_TIMESTAMP
                       WHERE profile_key=? AND remote_id=?""",
                    (state, code, message, key, int(row["id"])),
                )
            continue
        if existing["state"] == "completed" and reprobe_seconds > 0:
            db.execute(
                """UPDATE runtime_remote_schedule_state
                   SET state='eligible',reason_code='scheduled_reprobe',
                       reason_message='completed remote observation reached reprobe interval',
                       attempt_count=0,updated_at=CURRENT_TIMESTAMP
                   WHERE profile_key=? AND remote_id=? AND state='completed'
                     AND last_attempt_at<=datetime('now',?)""",
                (key, int(row["id"]), f"-{reprobe_seconds} seconds"),
            )
        elif existing["state"] not in {
            "running",
            "completed",
            "failed",
            "blocked",
            "inconclusive",
            "eligible",
        }:
            db.execute(
                """UPDATE runtime_remote_schedule_state
                   SET state='eligible',reason_code=NULL,reason_message=NULL,updated_at=CURRENT_TIMESTAMP
                   WHERE profile_key=? AND remote_id=?""",
                (key, int(row["id"])),
            )


def previous_run(
    db: sqlite3.Connection, current_run: int, declared_url: str, probe_sha: str
) -> int | None:
    row = db.execute(
        """SELECT id FROM runtime_remote_observation_runs
           WHERE status='completed' AND id<? AND declared_url=? AND probe_profile_sha256=?
           ORDER BY id DESC LIMIT 1""",
        (current_run, declared_url, probe_sha),
    ).fetchone()
    return None if row is None else int(row["id"])


def compare_tools(
    db: sqlite3.Connection, older: int, newer: int
) -> dict[str, list[str]]:
    def load(run_id: int) -> dict[str, str]:
        return {
            str(row["name"]): str(row["definition_json"])
            for row in db.execute(
                "SELECT name,definition_json FROM runtime_remote_observation_tools WHERE run_id=?",
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


def _run_probe(
    args: argparse.Namespace, database: Path, remote_id: int, probe_sha: str
) -> tuple[str, str | None]:
    child_env = {
        "PATH": os.environ.get("PATH", "/usr/bin:/bin"),
        "LANG": "C.UTF-8",
    }
    if tmpdir := os.environ.get("TMPDIR"):
        child_env["TMPDIR"] = tmpdir
    try:
        child = subprocess.run(
            [
                sys.executable,
                str(Path(args.remote_runner).resolve()),
                "--database",
                str(database),
                "--remote-id",
                str(remote_id),
                "--timeout",
                str(args.phase_timeout_seconds),
                "--probe-profile-sha256",
                probe_sha,
            ],
            stdin=subprocess.DEVNULL,
            capture_output=True,
            timeout=args.child_timeout_seconds,
            check=False,
            env=child_env,
        )
        return child.stderr.decode("utf-8", "replace")[:2048], None
    except subprocess.TimeoutExpired:
        return "remote runner exceeded child timeout", "remote_child_timeout"


def execute(args: argparse.Namespace) -> dict[str, Any]:
    database = Path(args.database).resolve()
    profile, key = load_profile(args)
    db = connect(database)
    try:
        with db:
            db.execute(
                """INSERT INTO runtime_remote_schedule_profiles(
                     profile_key,scheduler_version,probe_profile_sha256,runner_sha256)
                   VALUES(?,?,?,?) ON CONFLICT(profile_key) DO UPDATE SET selected_at=CURRENT_TIMESTAMP""",
                (
                    key,
                    profile["scheduler_version"],
                    profile["probe_profile_sha256"],
                    profile["runner_sha256"],
                ),
            )
            db.execute(
                """INSERT INTO runtime_remote_schedule_current(singleton,profile_key) VALUES(1,?)
                   ON CONFLICT(singleton) DO UPDATE SET profile_key=excluded.profile_key,updated_at=CURRENT_TIMESTAMP""",
                (key,),
            )
            synchronize(
                db,
                key,
                args.reprobe_completed_after_seconds,
                args.stale_running_after_seconds,
            )

        started = time.monotonic()
        processed = 0
        while (
            processed < args.batch_size
            and time.monotonic() - started < args.maximum_run_seconds
        ):
            row = db.execute(
                """SELECT s.remote_id,r.server_version_id,r.url
                   FROM runtime_remote_schedule_state s JOIN remotes r ON r.id=s.remote_id
                   WHERE s.profile_key=? AND s.state IN('eligible','failed','inconclusive')
                     AND s.attempt_count<?
                     AND (s.last_attempt_at IS NULL OR s.last_attempt_at<=datetime('now',?))
                   ORDER BY s.discovered_at DESC,s.attempt_count,s.remote_id DESC LIMIT 1""",
                (
                    key,
                    args.maximum_attempts,
                    f"-{args.retry_failed_after_seconds} seconds",
                ),
            ).fetchone()
            if row is None:
                break
            remote_id = int(row["remote_id"])
            with db:
                db.execute(
                    """UPDATE runtime_remote_schedule_state
                       SET state='running',attempt_count=attempt_count+1,
                           last_attempt_at=CURRENT_TIMESTAMP,reason_code=NULL,reason_message=NULL,
                           runtime_remote_observation_run_id=NULL,updated_at=CURRENT_TIMESTAMP
                       WHERE profile_key=? AND remote_id=?""",
                    (key, remote_id),
                )

            child_stderr, child_failure = _run_probe(
                args, database, remote_id, profile["probe_profile_sha256"]
            )
            observation = db.execute(
                """SELECT id,status,inventory_sha256,error_stage,error_message
                   FROM runtime_remote_observation_runs WHERE remote_id=? ORDER BY id DESC LIMIT 1""",
                (remote_id,),
            ).fetchone()
            state = "inconclusive"
            code = child_failure or "remote_runner_failed"
            message: str | None = child_stderr
            run_id = None
            inventory_sha = None
            previous = None
            drift = None

            if child_failure is not None and observation is not None and observation["status"] == "running":
                run_id = int(observation["id"])
                with db:
                    db.execute(
                        """UPDATE runtime_remote_observation_runs
                           SET status='inconclusive',completed_at=CURRENT_TIMESTAMP,
                               error_stage=?,error_message=? WHERE id=? AND status='running'""",
                        (child_failure, child_stderr, run_id),
                    )
            elif child_failure is None and observation is not None:
                run_id = int(observation["id"])
                inventory_sha = observation["inventory_sha256"]
                observed_state = str(observation["status"])
                state = (
                    observed_state
                    if observed_state in {"completed", "failed", "blocked", "inconclusive"}
                    else "inconclusive"
                )
                code = (
                    None
                    if state == "completed"
                    else str(observation["error_stage"] or "remote_probe_failed")
                )
                message = (
                    None
                    if state == "completed"
                    else str(observation["error_message"] or "")
                )
                if state == "completed":
                    previous = previous_run(
                        db,
                        run_id,
                        str(row["url"]),
                        profile["probe_profile_sha256"],
                    )
                    if previous is not None:
                        drift = compare_tools(db, previous, run_id)
            counts = {
                name: None for name in ("added", "removed", "modified", "unchanged")
            }
            if drift is not None:
                counts = {name: len(drift[name]) for name in counts}
            with db:
                db.execute(
                    """UPDATE runtime_remote_schedule_state
                       SET state=?,reason_code=?,reason_message=?,
                           runtime_remote_observation_run_id=?,inventory_sha256=?,
                           previous_compatible_run_id=?,added_tools=?,removed_tools=?,
                           modified_tools=?,unchanged_tools=?,updated_at=CURRENT_TIMESTAMP
                       WHERE profile_key=? AND remote_id=?""",
                    (
                        state,
                        code,
                        message,
                        run_id,
                        inventory_sha,
                        previous,
                        counts["added"],
                        counts["removed"],
                        counts["modified"],
                        counts["unchanged"],
                        key,
                        remote_id,
                    ),
                )
            processed += 1

        summary = db.execute(
            """SELECT COUNT(*) total,
                      SUM(state IN('eligible','running','completed','failed','blocked','inconclusive')) eligible,
                      SUM(state='completed') completed,SUM(state='failed') failed,
                      SUM(state='blocked') blocked,SUM(state='inconclusive') inconclusive,
                      SUM(state IN('unsupported','unresolvable')) unsupported,
                      SUM(state='completed' AND previous_compatible_run_id IS NOT NULL) comparable,
                      SUM(state='completed' AND previous_compatible_run_id IS NOT NULL AND
                        (COALESCE(added_tools,0)+COALESCE(removed_tools,0)+COALESCE(modified_tools,0))>0) drifted
               FROM runtime_remote_schedule_state WHERE profile_key=?""",
            (key,),
        ).fetchone()
        result = {name: int(summary[name] or 0) for name in summary.keys()}
        result.update(
            {
                "profile_key": key,
                "processed_in_batch": processed,
                "run_elapsed_seconds": round(time.monotonic() - started, 3),
            }
        )
        return result
    finally:
        db.close()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--database", required=True)
    parser.add_argument("--remote-runner", required=True)
    parser.add_argument("--probe-profile", required=True)
    parser.add_argument("--batch-size", type=int, default=5)
    parser.add_argument("--maximum-run-seconds", type=int, default=300)
    parser.add_argument("--maximum-attempts", type=int, default=3)
    parser.add_argument("--retry-failed-after-seconds", type=int, default=86400)
    parser.add_argument("--reprobe-completed-after-seconds", type=int, default=86400)
    parser.add_argument("--stale-running-after-seconds", type=int, default=7200)
    parser.add_argument("--phase-timeout-seconds", type=int, default=15)
    parser.add_argument("--child-timeout-seconds", type=int, default=45)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.batch_size < 1 or args.batch_size > 100:
        raise ValueError("batch size must be between 1 and 100")
    if args.maximum_run_seconds < 1:
        raise ValueError("maximum run seconds must be positive")
    if args.reprobe_completed_after_seconds < 3600:
        raise ValueError("remote reprobe interval must be at least one hour")
    if args.stale_running_after_seconds < 60:
        raise ValueError("stale running interval must be at least one minute")
    print(canonical(execute(args)))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, sqlite3.Error, ValueError) as exc:
        print(f"remote runtime scheduler failed: {exc}", file=sys.stderr)
        raise SystemExit(2)
