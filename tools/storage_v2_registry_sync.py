#!/usr/bin/env python3
"""Synchronize immutable Registry identity rows from Storage v2 hot to history.

The compact hot catalog is the Registry-refresh target.  After a successful hot
refresh, this tool appends the same Registry identities to the history/control
database so static/runtime observations can reference the newly published
package rows.  Existing rows are validated and never replaced.
"""

from __future__ import annotations

import argparse
from contextlib import contextmanager
import fcntl
import os
from pathlib import Path
import sqlite3
from typing import Iterator

TABLES = (
    "snapshots",
    "server_versions",
    "snapshot_server_versions",
    "repositories",
    "packages",
    "package_arguments",
    "package_environment",
    "remotes",
)


@contextmanager
def writer_lock(database: Path) -> Iterator[None]:
    descriptor = os.open(
        str(database) + ".writer.lock",
        os.O_RDWR | os.O_CREAT | os.O_CLOEXEC | os.O_NOFOLLOW,
        0o600,
    )
    try:
        fcntl.flock(descriptor, fcntl.LOCK_EX)
        yield
    finally:
        os.close(descriptor)


def connect(path: Path, readonly: bool = False) -> sqlite3.Connection:
    if readonly:
        db = sqlite3.connect(f"file:{path.resolve().as_posix()}?mode=ro", uri=True, timeout=30)
        db.execute("PRAGMA query_only=ON")
    else:
        db = sqlite3.connect(path, timeout=30)
    db.row_factory = sqlite3.Row
    db.execute("PRAGMA foreign_keys=ON")
    db.execute("PRAGMA busy_timeout=30000")
    return db


def columns(db: sqlite3.Connection, table: str) -> list[str]:
    return [str(row["name"]) for row in db.execute(f"PRAGMA table_info({table})")]


def table_exists(db: sqlite3.Connection, table: str) -> bool:
    return db.execute(
        "SELECT 1 FROM sqlite_schema WHERE type='table' AND name=?", (table,)
    ).fetchone() is not None


def primary_key_columns(db: sqlite3.Connection, table: str) -> list[str]:
    rows = list(db.execute(f"PRAGMA table_info({table})"))
    return [
        str(row["name"])
        for row in sorted((row for row in rows if int(row["pk"]) > 0), key=lambda row: int(row["pk"]))
    ]


def sync_table(source: sqlite3.Connection, target: sqlite3.Connection, table: str) -> tuple[int, int]:
    if not table_exists(source, table) or not table_exists(target, table):
        raise RuntimeError(f"required Registry table missing: {table}")
    names = columns(source, table)
    if names != columns(target, table):
        raise RuntimeError(f"Registry schema mismatch for {table}")
    pk = primary_key_columns(source, table)
    if not pk:
        raise RuntimeError(f"Registry table has no primary key: {table}")

    projection = ",".join(names)
    placeholders = ",".join("?" for _ in names)
    inserted = 0
    validated = 0
    for row in source.execute(f"SELECT {projection} FROM {table}"):
        where = " AND ".join(f"{name}=?" for name in pk)
        key = tuple(row[name] for name in pk)
        existing = target.execute(
            f"SELECT {projection} FROM {table} WHERE {where}", key
        ).fetchone()
        values = tuple(row[name] for name in names)
        if existing is None:
            target.execute(
                f"INSERT INTO {table}({projection}) VALUES({placeholders})", values
            )
            inserted += 1
        else:
            current = tuple(existing[name] for name in names)
            if current != values:
                raise RuntimeError(
                    f"immutable Registry row differs between hot/history: {table} key={key}"
                )
            validated += 1
    return inserted, validated


def sync(hot: Path, history: Path) -> dict[str, dict[str, int]]:
    with writer_lock(history):
        source = connect(hot, readonly=True)
        target = connect(history)
        try:
            result: dict[str, dict[str, int]] = {}
            with target:
                for table in TABLES:
                    inserted, validated = sync_table(source, target, table)
                    result[table] = {"inserted": inserted, "validated": validated}
            check = target.execute("PRAGMA foreign_key_check").fetchall()
            if check:
                raise RuntimeError(f"history foreign_key_check failed after Registry sync: {check[:3]}")
            return result
        finally:
            target.close()
            source.close()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--hot", required=True, type=Path)
    parser.add_argument("--history", required=True, type=Path)
    args = parser.parse_args()
    result = sync(args.hot.resolve(), args.history.resolve())
    for table, counts in result.items():
        print(f"{table}: inserted={counts['inserted']} validated={counts['validated']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
