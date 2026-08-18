#!/usr/bin/env python3
"""Install and verify Storage-v2 history indexes used by Coverage drill-downs."""

from __future__ import annotations

import argparse
from contextlib import contextmanager
import fcntl
import json
import os
from pathlib import Path
import sqlite3
from typing import Iterator

INDEXES = {
    "static_schedule_profile_state_updated": """
        CREATE INDEX IF NOT EXISTS static_schedule_profile_state_updated
        ON static_analysis_schedule_state(
            profile_key,
            state,
            updated_at COLLATE BINARY DESC,
            package_id DESC
        )
    """,
    "static_schedule_profile_attempt_updated": """
        CREATE INDEX IF NOT EXISTS static_schedule_profile_attempt_updated
        ON static_analysis_schedule_state(
            profile_key,
            state,
            attempt_count,
            updated_at COLLATE BINARY DESC,
            package_id DESC
        )
    """,
    "static_schedule_profile_updated": """
        CREATE INDEX IF NOT EXISTS static_schedule_profile_updated
        ON static_analysis_schedule_state(
            profile_key,
            updated_at COLLATE BINARY DESC,
            package_id DESC
        )
    """,
}

COVERAGE_COMPLETED_SQL = """SELECT package_id FROM static_analysis_schedule_state
WHERE profile_key='profile' AND state='completed'
ORDER BY updated_at COLLATE BINARY DESC, package_id DESC LIMIT 50"""
COVERAGE_NEVER_SQL = """SELECT package_id FROM static_analysis_schedule_state
WHERE profile_key='profile' AND state='eligible' AND attempt_count=0
ORDER BY updated_at COLLATE BINARY DESC, package_id DESC LIMIT 50"""
COVERAGE_ELIGIBLE_SQL = """SELECT package_id FROM static_analysis_schedule_state
WHERE profile_key='profile' AND state IN('eligible','running','completed','failed')
ORDER BY updated_at COLLATE BINARY DESC, package_id DESC LIMIT 50"""

@contextmanager
def writer_lock(database: Path) -> Iterator[None]:
    path = Path(str(database) + ".writer.lock")
    descriptor = os.open(path, os.O_RDWR | os.O_CREAT | os.O_CLOEXEC | os.O_NOFOLLOW, 0o600)
    try:
        fcntl.flock(descriptor, fcntl.LOCK_EX)
        yield
    finally:
        os.close(descriptor)

def connect(database: Path, *, readonly: bool = False) -> sqlite3.Connection:
    if readonly:
        db = sqlite3.connect(f"file:{database.resolve().as_posix()}?mode=ro", uri=True, timeout=30)
        db.execute("PRAGMA query_only=ON")
    else:
        db = sqlite3.connect(database, timeout=30)
    db.row_factory = sqlite3.Row
    db.execute("PRAGMA busy_timeout=30000")
    return db

def require_schema(db: sqlite3.Connection) -> None:
    expected = {"profile_key", "package_id", "state", "attempt_count", "updated_at"}
    columns = {str(row["name"]) for row in db.execute("PRAGMA table_info(static_analysis_schedule_state)")}
    missing = sorted(expected - columns)
    if missing:
        raise RuntimeError("static_analysis_schedule_state missing required columns: " + ", ".join(missing))

def install(database: Path) -> None:
    with writer_lock(database):
        db = connect(database)
        try:
            require_schema(db)
            with db:
                for sql in INDEXES.values():
                    db.execute(sql)
            db.execute("PRAGMA optimize")
        finally:
            db.close()

def _plan(db: sqlite3.Connection, sql: str) -> list[str]:
    return [str(row[3]) for row in db.execute("EXPLAIN QUERY PLAN " + sql).fetchall()]

def status(database: Path) -> dict[str, object]:
    db = connect(database, readonly=True)
    try:
        require_schema(db)
        names = {str(row["name"]) for row in db.execute("PRAGMA index_list('static_analysis_schedule_state')")}
        installed = {name: name in names for name in INDEXES}
        plans = {
            "coverage_completed": _plan(db, COVERAGE_COMPLETED_SQL),
            "coverage_never": _plan(db, COVERAGE_NEVER_SQL),
            "coverage_eligible": _plan(db, COVERAGE_ELIGIBLE_SQL),
        }
        uses = {
            "coverage_completed": any("static_schedule_profile_state_updated" in step for step in plans["coverage_completed"]),
            "coverage_never": any("static_schedule_profile_attempt_updated" in step for step in plans["coverage_never"]),
            "coverage_eligible": any("static_schedule_profile_updated" in step for step in plans["coverage_eligible"]),
        }
        return {"database": str(database.resolve()), "installed": installed,
                "query_plans": plans, "uses_expected_index": uses}
    finally:
        db.close()

def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--database", required=True, type=Path)
    parser.add_argument("--verify-only", action="store_true")
    return parser.parse_args()

def main() -> int:
    args = parse_args()
    database = args.database.resolve()
    if not database.is_file():
        raise FileNotFoundError(database)
    if not args.verify_only:
        install(database)
    result = status(database)
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0 if all(result["installed"].values()) and all(result["uses_expected_index"].values()) else 2

if __name__ == "__main__":
    raise SystemExit(main())
