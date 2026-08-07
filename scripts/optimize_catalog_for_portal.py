#!/usr/bin/env python3
"""Install bounded read indexes used by the public portal.

This maintenance command never rewrites research rows. It serializes with the
existing adjacent catalog writer lock, creates only idempotent indexes, and asks
SQLite to refresh planner statistics with PRAGMA optimize.
"""

from __future__ import annotations

import argparse
import fcntl
import os
from pathlib import Path
import sqlite3
import time
from typing import Iterable


INDEXES: dict[str, tuple[str, ...]] = {
    "analysis_runs": (
        "CREATE INDEX IF NOT EXISTS portal_analysis_runs_status "
        "ON analysis_runs(status)",
        "CREATE INDEX IF NOT EXISTS portal_analysis_runs_started "
        "ON analysis_runs(started_at DESC, id DESC)",
    ),
    "analysis_findings": (
        "CREATE INDEX IF NOT EXISTS portal_analysis_findings_disposition_severity "
        "ON analysis_findings(disposition, severity)",
    ),
    "static_analysis_schedule_state": (
        "CREATE INDEX IF NOT EXISTS portal_static_schedule_coverage "
        "ON static_analysis_schedule_state(profile_key, state, artifact_sha256)",
    ),
    "runtime_observation_runs": (
        "CREATE INDEX IF NOT EXISTS portal_runtime_completed_packages "
        "ON runtime_observation_runs(status, package_id)",
    ),
}


def table_names(connection: sqlite3.Connection) -> set[str]:
    return {
        str(row[0])
        for row in connection.execute(
            "SELECT name FROM sqlite_schema WHERE type='table'"
        )
    }


def applicable_statements(tables: set[str]) -> Iterable[str]:
    for table, statements in INDEXES.items():
        if table in tables:
            yield from statements


def optimize(database: Path, busy_timeout_ms: int) -> list[str]:
    lock_path = Path(str(database) + ".writer.lock")
    descriptor = os.open(
        lock_path,
        os.O_RDWR | os.O_CREAT | os.O_CLOEXEC | os.O_NOFOLLOW,
        0o600,
    )
    try:
        while True:
            try:
                fcntl.flock(descriptor, fcntl.LOCK_EX)
                break
            except InterruptedError:
                continue

        connection = sqlite3.connect(database, timeout=busy_timeout_ms / 1000)
        try:
            connection.execute("PRAGMA foreign_keys=ON")
            connection.execute(f"PRAGMA busy_timeout={busy_timeout_ms}")
            tables = table_names(connection)
            created: list[str] = []
            with connection:
                for statement in applicable_statements(tables):
                    connection.execute(statement)
                    created.append(statement.split()[5])
            connection.execute("PRAGMA optimize")
            return created
        finally:
            connection.close()
    finally:
        os.close(descriptor)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--database", required=True)
    parser.add_argument("--busy-timeout-ms", type=int, default=30_000)
    args = parser.parse_args()
    if args.busy_timeout_ms <= 0:
        parser.error("--busy-timeout-ms must be positive")
    return args


def main() -> int:
    args = parse_args()
    database = Path(args.database).resolve()
    if not database.is_file():
        raise SystemExit(f"catalog database does not exist: {database}")

    started = time.monotonic()
    indexes = optimize(database, args.busy_timeout_ms)
    elapsed = time.monotonic() - started
    print(f"database={database}")
    print(f"indexes_checked={len(indexes)}")
    print(f"elapsed_seconds={elapsed:.3f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
