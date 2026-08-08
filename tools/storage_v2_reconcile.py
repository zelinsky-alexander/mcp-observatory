#!/usr/bin/env python3
"""Recompute Storage v2 materialized coverage with canonical run semantics.

Package-state counters are computed over the current scheduler profile. Finding
and review counters are computed once per distinct completed analysis_run_id, so
one canonical analysis reused by multiple package records is not multiplied in
portal statistics.

This is intentionally separate from request-time portal queries and uses only
Python's standard library plus SQLite.
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


def connect(database: Path) -> sqlite3.Connection:
    db = sqlite3.connect(database, timeout=30)
    db.row_factory = sqlite3.Row
    db.execute("PRAGMA foreign_keys=ON")
    db.execute("PRAGMA busy_timeout=30000")
    return db


def require_tables(db: sqlite3.Connection) -> None:
    required = {
        "static_analysis_schedule_current",
        "static_analysis_schedule_state",
        "analysis_v2_run_summaries",
        "analysis_v2_coverage_summary",
    }
    existing = {
        str(row[0])
        for row in db.execute("SELECT name FROM sqlite_schema WHERE type='table'")
    }
    missing = sorted(required - existing)
    if missing:
        raise RuntimeError("missing Storage v2 coverage tables: " + ", ".join(missing))


def reconcile(db: sqlite3.Connection) -> dict[str, int | str]:
    require_tables(db)
    profile = db.execute(
        "SELECT profile_key FROM static_analysis_schedule_current WHERE singleton=1"
    ).fetchone()
    if profile is None:
        raise RuntimeError("no current static-analysis profile")
    key = str(profile["profile_key"])

    package = db.execute(
        """SELECT
             COALESCE(SUM(state IN('eligible','running','completed','failed')),0) eligible,
             COALESCE(SUM(state='completed'),0) completed,
             COALESCE(SUM(state='failed'),0) failed,
             COALESCE(SUM(state='unsupported'),0) unsupported,
             COALESCE(SUM(state='unresolvable'),0) unresolvable,
             COALESCE(SUM(state='eligible' AND attempt_count=0),0) never_attempted,
             COALESCE(SUM(state='running'),0) running,
             COUNT(DISTINCT CASE WHEN state='completed' THEN artifact_sha256 END) unique_artifacts
           FROM static_analysis_schedule_state WHERE profile_key=?""",
        (key,),
    ).fetchone()
    assert package is not None

    findings = db.execute(
        """SELECT
             COALESCE(SUM(s.info_count),0) info,
             COALESCE(SUM(s.low_count),0) low,
             COALESCE(SUM(s.medium_count),0) medium,
             COALESCE(SUM(s.high_count),0) high,
             COALESCE(SUM(s.critical_count),0) critical,
             COALESCE(SUM(s.unreviewed_count),0) unreviewed,
             COALESCE(SUM(s.unreviewed_high_count+s.unreviewed_critical_count),0)
               unreviewed_high_or_critical,
             COALESCE(SUM(s.suspicious_count),0) suspicious,
             COALESCE(SUM(s.confirmed_risk_count),0) confirmed_risk,
             COUNT(*) distinct_completed_runs
           FROM analysis_v2_run_summaries s
           JOIN (
             SELECT DISTINCT analysis_run_id
             FROM static_analysis_schedule_state
             WHERE profile_key=? AND state='completed' AND analysis_run_id IS NOT NULL
           ) selected ON selected.analysis_run_id=s.analysis_run_id""",
        (key,),
    ).fetchone()
    assert findings is not None

    values = (
        key,
        int(package["eligible"] or 0),
        int(package["completed"] or 0),
        int(package["failed"] or 0),
        int(package["unsupported"] or 0),
        int(package["unresolvable"] or 0),
        int(package["never_attempted"] or 0),
        int(package["running"] or 0),
        int(package["unique_artifacts"] or 0),
        int(findings["info"] or 0),
        int(findings["low"] or 0),
        int(findings["medium"] or 0),
        int(findings["high"] or 0),
        int(findings["critical"] or 0),
        int(findings["unreviewed"] or 0),
        int(findings["unreviewed_high_or_critical"] or 0),
        int(findings["suspicious"] or 0),
        int(findings["confirmed_risk"] or 0),
    )
    db.execute(
        """INSERT INTO analysis_v2_coverage_summary(
             profile_key,eligible_package_records,completed_package_records,
             failed_package_records,unsupported_package_records,
             unresolvable_package_records,never_attempted_package_records,
             running_package_records,unique_artifacts_analyzed,
             info_findings,low_findings,medium_findings,high_findings,critical_findings,
             unreviewed_findings,unreviewed_high_or_critical_findings,
             suspicious_findings,confirmed_risk_findings)
           VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
           ON CONFLICT(profile_key) DO UPDATE SET
             eligible_package_records=excluded.eligible_package_records,
             completed_package_records=excluded.completed_package_records,
             failed_package_records=excluded.failed_package_records,
             unsupported_package_records=excluded.unsupported_package_records,
             unresolvable_package_records=excluded.unresolvable_package_records,
             never_attempted_package_records=excluded.never_attempted_package_records,
             running_package_records=excluded.running_package_records,
             unique_artifacts_analyzed=excluded.unique_artifacts_analyzed,
             info_findings=excluded.info_findings,
             low_findings=excluded.low_findings,
             medium_findings=excluded.medium_findings,
             high_findings=excluded.high_findings,
             critical_findings=excluded.critical_findings,
             unreviewed_findings=excluded.unreviewed_findings,
             unreviewed_high_or_critical_findings=excluded.unreviewed_high_or_critical_findings,
             suspicious_findings=excluded.suspicious_findings,
             confirmed_risk_findings=excluded.confirmed_risk_findings,
             updated_at=CURRENT_TIMESTAMP""",
        values,
    )
    return {
        "profile_key": key,
        "eligible_package_records": values[1],
        "completed_package_records": values[2],
        "unique_artifacts_analyzed": values[8],
        "distinct_completed_analysis_runs": int(findings["distinct_completed_runs"] or 0),
        "finding_occurrences": sum(values[9:14]),
        "unreviewed_high_or_critical_findings": values[15],
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--database", required=True, type=Path)
    return parser.parse_args()


def main() -> int:
    database = parse_args().database.resolve()
    with writer_lock(database):
        db = connect(database)
        try:
            with db:
                result = reconcile(db)
        finally:
            db.close()
    print(json.dumps(result, sort_keys=True, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, sqlite3.Error, RuntimeError) as exc:
        print(f"storage v2 coverage reconcile failed: {exc}", file=__import__("sys").stderr)
        raise SystemExit(2)
