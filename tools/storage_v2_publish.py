#!/usr/bin/env python3
"""Build an atomic compact portal catalog from a Storage v2 history database.

The source is opened read-only and copied with SQLite's backup API. Compaction
happens only in a temporary destination. Registry state, analysis run identity,
Storage v2 summaries, runtime observations, and actionable static findings are
kept. Large per-file inventories, dependencies, and legacy evidence-index rows
are removed from the hot copy. The full history database remains authoritative.

No third-party dependencies are used.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import sqlite3
import time

REQUIRED_V2_TABLES = {
    "storage_v2_info",
    "analysis_v2_rule_definitions",
    "analysis_v2_run_summaries",
    "analysis_v2_rule_summaries",
    "analysis_v2_evidence_manifests",
    "analysis_v2_coverage_summary",
}


class PublishError(RuntimeError):
    pass


def ro_connection(path: Path) -> sqlite3.Connection:
    uri = f"file:{path.as_posix()}?mode=ro"
    db = sqlite3.connect(uri, uri=True, timeout=30)
    db.row_factory = sqlite3.Row
    db.execute("PRAGMA query_only=ON")
    db.execute("PRAGMA busy_timeout=30000")
    return db


def rw_connection(path: Path) -> sqlite3.Connection:
    db = sqlite3.connect(path, timeout=30)
    db.row_factory = sqlite3.Row
    db.execute("PRAGMA foreign_keys=ON")
    db.execute("PRAGMA busy_timeout=30000")
    return db


def table_names(db: sqlite3.Connection) -> set[str]:
    return {
        str(row[0])
        for row in db.execute("SELECT name FROM sqlite_schema WHERE type='table'")
    }


def require_storage_v2(db: sqlite3.Connection) -> None:
    missing = sorted(REQUIRED_V2_TABLES - table_names(db))
    if missing:
        raise PublishError("source database lacks Storage v2 tables: " + ", ".join(missing))
    remaining = int(
        db.execute(
            """SELECT COUNT(*) FROM analysis_runs r
               LEFT JOIN analysis_v2_run_summaries s ON s.analysis_run_id=r.id
               WHERE s.analysis_run_id IS NULL"""
        ).fetchone()[0]
    )
    if remaining:
        raise PublishError(
            f"source database has {remaining} analysis runs without Storage v2 summaries"
        )


def count_if_present(db: sqlite3.Connection, table: str) -> int:
    if table not in table_names(db):
        return 0
    return int(db.execute(f"SELECT COUNT(*) FROM {table}").fetchone()[0])


def compact_hot(db: sqlite3.Connection, keep_actionable_findings: bool) -> dict[str, int]:
    present = table_names(db)
    before = {
        name: count_if_present(db, name)
        for name in (
            "analysis_files",
            "analysis_dependencies",
            "analysis_evidence",
            "analysis_findings",
            "analysis_finding_reviews",
        )
    }
    with db:
        if "analysis_files" in present:
            db.execute("DELETE FROM analysis_files")
        if "analysis_dependencies" in present:
            db.execute("DELETE FROM analysis_dependencies")
        if "analysis_evidence" in present:
            db.execute("DELETE FROM analysis_evidence")
        if "analysis_findings" in present:
            if keep_actionable_findings:
                db.execute(
                    """DELETE FROM analysis_findings
                       WHERE severity NOT IN('high','critical')
                         AND disposition NOT IN('suspicious','confirmed-risk')"""
                )
            else:
                db.execute("DELETE FROM analysis_findings")
        db.execute(
            """CREATE TABLE IF NOT EXISTS storage_v2_hot_catalog_info(
                 singleton INTEGER PRIMARY KEY CHECK(singleton=1),
                 built_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
                 detail_policy TEXT NOT NULL,
                 source_database_bytes INTEGER NOT NULL,
                 source_analysis_runs INTEGER NOT NULL,
                 source_analysis_findings INTEGER NOT NULL,
                 source_analysis_files INTEGER NOT NULL
               )"""
        )
        db.execute("DELETE FROM storage_v2_hot_catalog_info")
        db.execute(
            """INSERT INTO storage_v2_hot_catalog_info(
                 singleton,detail_policy,source_database_bytes,source_analysis_runs,
                 source_analysis_findings,source_analysis_files)
               VALUES(1,?,?,?,?,?)""",
            (
                "actionable-findings" if keep_actionable_findings else "summaries-only",
                0,
                count_if_present(db, "analysis_runs"),
                before["analysis_findings"],
                before["analysis_files"],
            ),
        )
    after = {name: count_if_present(db, name) for name in before}
    return {f"before_{k}": v for k, v in before.items()} | {
        f"after_{k}": v for k, v in after.items()
    }


def validate(db: sqlite3.Connection) -> dict[str, object]:
    check = db.execute("PRAGMA integrity_check").fetchone()
    if check is None or str(check[0]).lower() != "ok":
        raise PublishError(
            f"hot catalog integrity check failed: {check[0] if check else 'missing result'}"
        )
    required_registry = {
        "schema_info",
        "snapshots",
        "server_versions",
        "snapshot_server_versions",
        "packages",
    }
    missing = sorted((required_registry | REQUIRED_V2_TABLES) - table_names(db))
    if missing:
        raise PublishError("hot catalog is missing required tables: " + ", ".join(missing))
    coverage = int(db.execute("SELECT COUNT(*) FROM analysis_v2_coverage_summary").fetchone()[0])
    summaries = int(db.execute("SELECT COUNT(*) FROM analysis_v2_run_summaries").fetchone()[0])
    runs = count_if_present(db, "analysis_runs")
    if summaries != runs:
        raise PublishError(f"hot catalog summary/run mismatch: summaries={summaries} runs={runs}")
    return {
        "integrity": "ok",
        "analysis_runs": runs,
        "analysis_v2_run_summaries": summaries,
        "coverage_rows": coverage,
        "actionable_findings": count_if_present(db, "analysis_findings"),
    }


def publish(
    source_path: Path,
    output_path: Path,
    *,
    keep_actionable_findings: bool = True,
    vacuum: bool = True,
) -> dict[str, object]:
    source_path = source_path.resolve()
    output_path = output_path.resolve()
    if source_path == output_path:
        raise PublishError("source and output databases must be different")
    if not source_path.is_file():
        raise FileNotFoundError(source_path)

    output_path.parent.mkdir(parents=True, exist_ok=True)
    temporary = output_path.with_name(output_path.name + f".staging-{os.getpid()}")
    temporary.unlink(missing_ok=True)
    started = time.monotonic()

    source = ro_connection(source_path)
    destination = rw_connection(temporary)
    try:
        require_storage_v2(source)
        source_bytes = source_path.stat().st_size
        source.backup(destination, pages=4096, sleep=0.01)
        stats = compact_hot(destination, keep_actionable_findings)
        with destination:
            destination.execute(
                "UPDATE storage_v2_hot_catalog_info SET source_database_bytes=? WHERE singleton=1",
                (source_bytes,),
            )
        if vacuum:
            destination.execute("VACUUM")
        destination.execute("PRAGMA optimize")
        validation = validate(destination)
        destination.commit()
    except Exception:
        destination.close()
        source.close()
        temporary.unlink(missing_ok=True)
        raise
    else:
        destination.close()
        source.close()

    with temporary.open("rb") as handle:
        os.fsync(handle.fileno())
    os.replace(temporary, output_path)
    directory_fd = os.open(output_path.parent, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(directory_fd)
    finally:
        os.close(directory_fd)

    result: dict[str, object] = {
        "source": str(source_path),
        "output": str(output_path),
        "source_bytes": source_path.stat().st_size,
        "output_bytes": output_path.stat().st_size,
        "elapsed_seconds": round(time.monotonic() - started, 3),
        "detail_policy": "actionable-findings" if keep_actionable_findings else "summaries-only",
    }
    result.update(stats)
    result.update(validation)
    return result


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--summaries-only", action="store_true")
    parser.add_argument("--no-vacuum", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    result = publish(
        args.source,
        args.output,
        keep_actionable_findings=not args.summaries_only,
        vacuum=not args.no_vacuum,
    )
    print(json.dumps(result, sort_keys=True, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, sqlite3.Error, PublishError) as exc:
        print(f"storage v2 publish failed: {exc}", file=__import__("sys").stderr)
        raise SystemExit(2)
