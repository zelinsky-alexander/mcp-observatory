#!/usr/bin/env python3
"""Install and verify compact Storage-v2 indexes used by public list routes.

The hot Storage-v2 catalog retains server/version/package/run metadata while
large finding/file/evidence detail stays in history.  These additive indexes
support bounded public navigation without changing authoritative records.
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

INDEXES = {
    "server_versions_recent": """
        CREATE INDEX IF NOT EXISTS server_versions_recent
        ON server_versions(
            COALESCE(updated_at,published_at,'') COLLATE BINARY DESC,
            id DESC
        )
    """,
    "server_versions_identity_recent": """
        CREATE INDEX IF NOT EXISTS server_versions_identity_recent
        ON server_versions(
            server_identifier COLLATE BINARY,
            COALESCE(updated_at,published_at,'') COLLATE BINARY DESC,
            id DESC
        )
    """,
    "analysis_runs_status_started": """
        CREATE INDEX IF NOT EXISTS analysis_runs_status_started
        ON analysis_runs(
            status,
            started_at COLLATE BINARY DESC,
            id DESC
        )
    """,
}

RECORDS_SQL = """
SELECT id
FROM server_versions
ORDER BY COALESCE(updated_at,published_at,'') COLLATE BINARY DESC,
         id DESC
LIMIT 50
"""

ANALYSES_SQL = """
SELECT ar.id
FROM analysis_runs ar
WHERE ar.status='completed'
ORDER BY ar.started_at COLLATE BINARY DESC, ar.id DESC
LIMIT 50
"""

ALL_SERVERS_SQL = """
WITH matching AS (
    SELECT id,server_identifier,server_version,updated_at,published_at,
           ROW_NUMBER() OVER (
               PARTITION BY server_identifier
               ORDER BY COALESCE(updated_at,published_at,'') COLLATE BINARY DESC,
                        id DESC
           ) AS rn
    FROM server_versions
)
SELECT id
FROM matching
WHERE rn=1
ORDER BY COALESCE(updated_at,published_at,'') COLLATE BINARY DESC,
         server_identifier COLLATE BINARY
LIMIT 50
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
    required = {
        "server_versions": {"id", "server_identifier", "updated_at", "published_at"},
        "analysis_runs": {"id", "status", "started_at"},
    }
    for table, expected in required.items():
        columns = {str(row["name"]) for row in db.execute(f"PRAGMA table_info({table})")}
        missing = sorted(expected - columns)
        if missing:
            raise RuntimeError(f"{table} missing required columns: " + ", ".join(missing))


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
        server_indexes = {str(row["name"]) for row in db.execute("PRAGMA index_list('server_versions')")}
        run_indexes = {str(row["name"]) for row in db.execute("PRAGMA index_list('analysis_runs')")}
        installed = {
            name: (name in server_indexes or name in run_indexes)
            for name in INDEXES
        }
        plans = {
            "records": _plan(db, RECORDS_SQL),
            "analyses": _plan(db, ANALYSES_SQL),
            "all_servers": _plan(db, ALL_SERVERS_SQL),
        }
        uses = {
            "records": any("server_versions_recent" in step for step in plans["records"]),
            "analyses": any("analysis_runs_status_started" in step for step in plans["analyses"]),
            "all_servers": any("server_versions_identity_recent" in step for step in plans["all_servers"]),
        }
        return {
            "database": str(database.resolve()),
            "installed": installed,
            "query_plans": plans,
            "uses_expected_index": uses,
        }
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
    all_installed = all(bool(value) for value in result["installed"].values())
    all_used = all(bool(value) for value in result["uses_expected_index"].values())
    return 0 if all_installed and all_used else 2


if __name__ == "__main__":
    raise SystemExit(main())
