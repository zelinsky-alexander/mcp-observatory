#!/usr/bin/env python3
"""Mirror bounded runtime-coverage scheduler state from v2 history to hot catalog.

The history database remains authoritative. This helper publishes only the small
scheduler/profile read model needed by the public coverage and drift pages. Runtime
observation rows themselves continue to be published by ``storage_v2_mvp.py``.
"""

from __future__ import annotations

import argparse
from contextlib import contextmanager
import fcntl
import importlib.util
import json
import os
from pathlib import Path
import sqlite3
from typing import Iterator

TABLES = (
    "runtime_discovery_schedule_profiles",
    "runtime_discovery_schedule_current",
    "runtime_discovery_schedule_state",
)


@contextmanager
def writer_lock(database: Path) -> Iterator[None]:
    descriptor = os.open(
        Path(str(database) + ".writer.lock"),
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


def connect(path: Path, *, readonly: bool = False) -> sqlite3.Connection:
    if readonly:
        db = sqlite3.connect(f"file:{path.resolve().as_posix()}?mode=ro", uri=True, timeout=30)
        db.execute("PRAGMA query_only=ON")
    else:
        db = sqlite3.connect(path, timeout=30)
    db.row_factory = sqlite3.Row
    db.execute("PRAGMA foreign_keys=ON")
    db.execute("PRAGMA busy_timeout=30000")
    return db


def load_scheduler_schema() -> str:
    path = Path(__file__).with_name("bulk_runtime_discovery.py")
    spec = importlib.util.spec_from_file_location("bulk_runtime_discovery", path)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load bulk_runtime_discovery.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return str(module.SCHEMA)


def table_exists(db: sqlite3.Connection, name: str) -> bool:
    return db.execute(
        "SELECT 1 FROM sqlite_schema WHERE type='table' AND name=?", (name,)
    ).fetchone() is not None


def columns(db: sqlite3.Connection, name: str) -> list[str]:
    return [str(row["name"]) for row in db.execute(f"PRAGMA table_info({name})")]


def mirror(history: Path, hot: Path) -> dict[str, int]:
    with writer_lock(hot):
        source = connect(history, readonly=True)
        target = connect(hot)
        try:
            if not all(table_exists(source, name) for name in TABLES):
                return {name: 0 for name in TABLES}
            with target:
                target.executescript(load_scheduler_schema())
                for name in TABLES:
                    if columns(source, name) != columns(target, name):
                        raise RuntimeError(f"runtime scheduler schema mismatch while publishing {name}")

                # Delete children first, then rebuild the bounded read model.
                target.execute("DELETE FROM runtime_discovery_schedule_state")
                target.execute("DELETE FROM runtime_discovery_schedule_current")
                target.execute("DELETE FROM runtime_discovery_schedule_profiles")

                counts: dict[str, int] = {}
                for name in TABLES:
                    names = columns(source, name)
                    fields = ",".join(names)
                    placeholders = ",".join("?" for _ in names)
                    rows = source.execute(f"SELECT {fields} FROM {name}").fetchall()
                    if rows:
                        target.executemany(
                            f"INSERT INTO {name}({fields}) VALUES({placeholders})",
                            [tuple(row[field] for field in names) for row in rows],
                        )
                    counts[name] = len(rows)
            return counts
        finally:
            target.close()
            source.close()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--history", required=True, type=Path)
    parser.add_argument("--hot", required=True, type=Path)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    result = mirror(args.history, args.hot)
    print(json.dumps(result, sort_keys=True, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, sqlite3.Error, RuntimeError) as exc:
        print(f"runtime schedule publish failed: {exc}", file=__import__("sys").stderr)
        raise SystemExit(2)
