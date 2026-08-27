#!/usr/bin/env python3
"""Mirror bounded local and declared-remote runtime read models from history to hot.

The history database remains authoritative. The hot catalog receives only runtime
observations, canonical tool definitions, and scheduler/profile state needed by
public coverage and drift views.
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
from typing import Any, Iterator

RUNTIME_TABLES = (
    "runtime_observation_runs",
    "runtime_observation_tools",
)
SCHEDULE_TABLES = (
    "runtime_discovery_schedule_profiles",
    "runtime_discovery_schedule_current",
    "runtime_discovery_schedule_state",
)
REMOTE_RUNTIME_TABLES = (
    "runtime_remote_observation_runs",
    "runtime_remote_observation_tools",
)
REMOTE_SCHEDULE_TABLES = (
    "runtime_remote_schedule_profiles",
    "runtime_remote_schedule_current",
    "runtime_remote_schedule_state",
)
INSERT_ORDER = (
    *RUNTIME_TABLES,
    *SCHEDULE_TABLES,
    *REMOTE_RUNTIME_TABLES,
    *REMOTE_SCHEDULE_TABLES,
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
        db = sqlite3.connect(
            f"file:{path.resolve().as_posix()}?mode=ro", uri=True, timeout=30
        )
        db.execute("PRAGMA query_only=ON")
    else:
        db = sqlite3.connect(path, timeout=30)
    db.row_factory = sqlite3.Row
    db.execute("PRAGMA foreign_keys=ON")
    db.execute("PRAGMA busy_timeout=30000")
    return db


def load_module(filename: str, module_name: str) -> Any:
    path = Path(__file__).with_name(filename)
    spec = importlib.util.spec_from_file_location(module_name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {filename}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def load_schema(filename: str, module_name: str) -> str:
    return str(load_module(filename, module_name).SCHEMA)


def table_exists(db: sqlite3.Connection, name: str) -> bool:
    return (
        db.execute(
            "SELECT 1 FROM sqlite_schema WHERE type='table' AND name=?", (name,)
        ).fetchone()
        is not None
    )


def columns(db: sqlite3.Connection, name: str) -> list[str]:
    return [str(row["name"]) for row in db.execute(f"PRAGMA table_info({name})")]


def copy_table(
    source: sqlite3.Connection, target: sqlite3.Connection, name: str
) -> int:
    names = columns(source, name)
    if names != columns(target, name):
        raise RuntimeError(f"runtime schema mismatch while publishing {name}")
    fields = ",".join(names)
    placeholders = ",".join("?" for _ in names)
    rows = source.execute(f"SELECT {fields} FROM {name}").fetchall()
    if rows:
        target.executemany(
            f"INSERT INTO {name}({fields}) VALUES({placeholders})",
            [tuple(row[field] for field in names) for row in rows],
        )
    return len(rows)


def _schemas(target: sqlite3.Connection, *, include_remote: bool) -> None:
    target.executescript(load_schema("runtime_discovery.py", "runtime_discovery"))
    target.executescript(
        load_schema("bulk_runtime_discovery.py", "bulk_runtime_discovery")
    )
    auto_scheduler = load_module(
        "bulk_runtime_discovery_auto.py", "bulk_runtime_discovery_auto_publish"
    )
    auto_scheduler.ensure_outcome_states(target)
    if include_remote:
        if not table_exists(target, "remotes"):
            raise RuntimeError(
                "remote runtime publication requires the hot catalog remotes table"
            )
        target.executescript(
            load_schema("remote_runtime_discovery.py", "remote_runtime_discovery")
        )
        target.executescript(
            load_schema(
                "bulk_remote_runtime_discovery.py", "bulk_remote_runtime_discovery"
            )
        )


def mirror(history: Path, hot: Path) -> dict[str, int]:
    with writer_lock(hot):
        source = connect(history, readonly=True)
        target = connect(hot)
        try:
            if not all(table_exists(source, name) for name in RUNTIME_TABLES):
                return {name: 0 for name in INSERT_ORDER}

            schedule_available = all(
                table_exists(source, name) for name in SCHEDULE_TABLES
            )
            remote_runtime_available = all(
                table_exists(source, name) for name in REMOTE_RUNTIME_TABLES
            )
            remote_schedule_available = all(
                table_exists(source, name) for name in REMOTE_SCHEDULE_TABLES
            )
            include_remote = remote_runtime_available or remote_schedule_available

            with target:
                _schemas(target, include_remote=include_remote)

                for name in RUNTIME_TABLES:
                    if columns(source, name) != columns(target, name):
                        raise RuntimeError(
                            f"runtime schema mismatch while publishing {name}"
                        )
                if schedule_available:
                    for name in SCHEDULE_TABLES:
                        if columns(source, name) != columns(target, name):
                            raise RuntimeError(
                                f"runtime scheduler schema mismatch while publishing {name}"
                            )
                if remote_runtime_available:
                    for name in REMOTE_RUNTIME_TABLES:
                        if columns(source, name) != columns(target, name):
                            raise RuntimeError(
                                f"remote runtime schema mismatch while publishing {name}"
                            )
                if remote_schedule_available:
                    for name in REMOTE_SCHEDULE_TABLES:
                        if columns(source, name) != columns(target, name):
                            raise RuntimeError(
                                f"remote runtime scheduler schema mismatch while publishing {name}"
                            )

                # Children first keeps foreign-key validity during a coherent
                # generation rebuild. Remote tables are optional for pre-feature
                # history/hot databases and are touched only when present.
                delete_order = (
                    "runtime_remote_schedule_state",
                    "runtime_remote_schedule_current",
                    "runtime_remote_schedule_profiles",
                    "runtime_remote_observation_tools",
                    "runtime_remote_observation_runs",
                    "runtime_discovery_schedule_state",
                    "runtime_discovery_schedule_current",
                    "runtime_discovery_schedule_profiles",
                    "runtime_observation_tools",
                    "runtime_observation_runs",
                )
                for name in delete_order:
                    if table_exists(target, name):
                        target.execute(f"DELETE FROM {name}")

                counts: dict[str, int] = {}
                for name in RUNTIME_TABLES:
                    counts[name] = copy_table(source, target, name)
                for name in SCHEDULE_TABLES:
                    counts[name] = (
                        copy_table(source, target, name) if schedule_available else 0
                    )
                for name in REMOTE_RUNTIME_TABLES:
                    counts[name] = (
                        copy_table(source, target, name)
                        if remote_runtime_available
                        else 0
                    )
                for name in REMOTE_SCHEDULE_TABLES:
                    counts[name] = (
                        copy_table(source, target, name)
                        if remote_schedule_available
                        else 0
                    )
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
    result = mirror(args.history.resolve(), args.hot.resolve())
    print(json.dumps(result, sort_keys=True, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, sqlite3.Error, RuntimeError) as exc:
        print(
            f"runtime read-model publish failed: {exc}",
            file=__import__("sys").stderr,
        )
        raise SystemExit(2)
