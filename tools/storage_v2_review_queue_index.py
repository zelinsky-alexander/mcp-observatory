#!/usr/bin/env python3
"""Install and verify the Storage-v2 history review-queue index.

The public portal gets the aggregate queue size from the compact hot database,
but bounded finding rows remain authoritative in the history database.  This
partial index lets the history database locate one queue page without scanning
all static-analysis findings.
"""

from __future__ import annotations

import argparse
from contextlib import contextmanager
import fcntl
import json
import os
from pathlib import Path
import sqlite3
from typing import Iterator

INDEX_NAME = "analysis_findings_unreviewed_high_critical_queue"
INDEX_SQL = f"""
CREATE INDEX IF NOT EXISTS {INDEX_NAME}
ON analysis_findings(
    severity COLLATE BINARY ASC,
    analysis_run_id DESC,
    id DESC
)
WHERE disposition='unreviewed'
  AND severity IN ('high','critical')
"""

PAGE_SQL = """
SELECT id
FROM analysis_findings
WHERE disposition='unreviewed'
  AND severity IN ('high','critical')
ORDER BY severity COLLATE BINARY ASC,
         analysis_run_id DESC,
         id DESC
LIMIT ? OFFSET ?
"""


@contextmanager
def writer_lock(database: Path) -> Iterator[None]:
    path = Path(str(database) + ".writer.lock")
    descriptor = os.open(
        path,
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
        yield
    finally:
        os.close(descriptor)


def connect(database: Path, *, readonly: bool = False) -> sqlite3.Connection:
    if readonly:
        uri = f"file:{database.resolve().as_posix()}?mode=ro"
        db = sqlite3.connect(uri, uri=True, timeout=30)
        db.execute("PRAGMA query_only=ON")
    else:
        db = sqlite3.connect(database, timeout=30)
    db.row_factory = sqlite3.Row
    db.execute("PRAGMA busy_timeout=30000")
    return db


def require_schema(db: sqlite3.Connection) -> None:
    columns = {
        str(row["name"])
        for row in db.execute("PRAGMA table_info(analysis_findings)")
    }
    required = {"id", "analysis_run_id", "severity", "disposition"}
    missing = sorted(required - columns)
    if missing:
        raise RuntimeError(
            "analysis_findings missing required columns: " + ", ".join(missing)
        )


def install(database: Path) -> None:
    with writer_lock(database):
        db = connect(database)
        try:
            require_schema(db)
            with db:
                db.execute(INDEX_SQL)
            db.execute("PRAGMA optimize")
        finally:
            db.close()


def status(database: Path) -> dict[str, object]:
    db = connect(database, readonly=True)
    try:
        require_schema(db)
        indexes = {
            str(row["name"])
            for row in db.execute("PRAGMA index_list('analysis_findings')")
        }
        plan = [
            str(row[3])
            for row in db.execute(
                "EXPLAIN QUERY PLAN " + PAGE_SQL,
                (50, 0),
            ).fetchall()
        ]
        return {
            "database": str(database.resolve()),
            "index": INDEX_NAME,
            "installed": INDEX_NAME in indexes,
            "query_plan": plan,
            "uses_index": any(INDEX_NAME in step for step in plan),
        }
    finally:
        db.close()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--database", required=True, type=Path)
    parser.add_argument(
        "--verify-only",
        action="store_true",
        help="do not modify the database; only report index/query-plan status",
    )
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
    if not result["installed"] or not result["uses_index"]:
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
