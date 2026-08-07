#!/usr/bin/env python3
"""Additive Storage v2 foundation for static-analysis results.

This module intentionally leaves the v1 analysis tables authoritative.  It adds
compact read-model tables plus SQLite triggers so existing writers dual-write
v1 detail and v2 summaries without changing the analyzer yet.

No third-party dependencies are used.
"""

from __future__ import annotations

import argparse
from contextlib import contextmanager
import fcntl
import os
from pathlib import Path
import sqlite3
from typing import Iterator

SCHEMA_VERSION = 1

DDL = r"""
CREATE TABLE IF NOT EXISTS storage_v2_info(
    singleton INTEGER PRIMARY KEY CHECK(singleton=1),
    schema_version INTEGER NOT NULL,
    installed_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE IF NOT EXISTS analysis_v2_rule_definitions(
    id INTEGER PRIMARY KEY,
    ruleset_version TEXT NOT NULL,
    rule_id TEXT NOT NULL,
    category TEXT NOT NULL,
    severity TEXT NOT NULL CHECK(severity IN ('info','low','medium','high','critical')),
    title TEXT NOT NULL,
    explanation TEXT NOT NULL,
    UNIQUE(ruleset_version, rule_id)
);

CREATE TABLE IF NOT EXISTS analysis_v2_run_summaries(
    analysis_run_id INTEGER PRIMARY KEY REFERENCES analysis_runs(id) ON DELETE CASCADE,
    artifact_sha256 TEXT,
    analyzer_name TEXT NOT NULL,
    analyzer_version TEXT NOT NULL,
    ruleset_version TEXT NOT NULL,
    status TEXT NOT NULL,
    file_count INTEGER NOT NULL DEFAULT 0 CHECK(file_count >= 0),
    total_file_bytes INTEGER NOT NULL DEFAULT 0 CHECK(total_file_bytes >= 0),
    dependency_count INTEGER NOT NULL DEFAULT 0 CHECK(dependency_count >= 0),
    finding_count INTEGER NOT NULL DEFAULT 0 CHECK(finding_count >= 0),
    info_count INTEGER NOT NULL DEFAULT 0 CHECK(info_count >= 0),
    low_count INTEGER NOT NULL DEFAULT 0 CHECK(low_count >= 0),
    medium_count INTEGER NOT NULL DEFAULT 0 CHECK(medium_count >= 0),
    high_count INTEGER NOT NULL DEFAULT 0 CHECK(high_count >= 0),
    critical_count INTEGER NOT NULL DEFAULT 0 CHECK(critical_count >= 0),
    executable_count INTEGER NOT NULL DEFAULT 0 CHECK(executable_count >= 0),
    native_binary_count INTEGER NOT NULL DEFAULT 0 CHECK(native_binary_count >= 0),
    generated_count INTEGER NOT NULL DEFAULT 0 CHECK(generated_count >= 0),
    minified_count INTEGER NOT NULL DEFAULT 0 CHECK(minified_count >= 0),
    unreviewed_count INTEGER NOT NULL DEFAULT 0 CHECK(unreviewed_count >= 0),
    suspicious_count INTEGER NOT NULL DEFAULT 0 CHECK(suspicious_count >= 0),
    confirmed_risk_count INTEGER NOT NULL DEFAULT 0 CHECK(confirmed_risk_count >= 0),
    updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
);
CREATE INDEX IF NOT EXISTS analysis_v2_run_summaries_recent
ON analysis_v2_run_summaries(status, analysis_run_id DESC);
CREATE INDEX IF NOT EXISTS analysis_v2_run_summaries_artifact
ON analysis_v2_run_summaries(artifact_sha256, analyzer_version, ruleset_version);

CREATE TABLE IF NOT EXISTS analysis_v2_rule_summaries(
    analysis_run_id INTEGER NOT NULL REFERENCES analysis_runs(id) ON DELETE CASCADE,
    rule_definition_id INTEGER NOT NULL REFERENCES analysis_v2_rule_definitions(id) ON DELETE RESTRICT,
    occurrence_count INTEGER NOT NULL DEFAULT 0 CHECK(occurrence_count >= 0),
    PRIMARY KEY(analysis_run_id, rule_definition_id)
);

CREATE TABLE IF NOT EXISTS analysis_v2_evidence_manifests(
    analysis_run_id INTEGER PRIMARY KEY REFERENCES analysis_runs(id) ON DELETE CASCADE,
    storage_kind TEXT NOT NULL CHECK(storage_kind IN ('legacy-directory','bundle','external-object')),
    locator TEXT NOT NULL,
    bundle_sha256 TEXT,
    inventory_sha256 TEXT,
    retained_artifact INTEGER NOT NULL DEFAULT 0 CHECK(retained_artifact IN (0,1)),
    updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE IF NOT EXISTS analysis_v2_coverage_summary(
    profile_key TEXT PRIMARY KEY,
    eligible_package_records INTEGER NOT NULL DEFAULT 0,
    completed_package_records INTEGER NOT NULL DEFAULT 0,
    failed_package_records INTEGER NOT NULL DEFAULT 0,
    unsupported_package_records INTEGER NOT NULL DEFAULT 0,
    unresolvable_package_records INTEGER NOT NULL DEFAULT 0,
    never_attempted_package_records INTEGER NOT NULL DEFAULT 0,
    unique_artifacts_analyzed INTEGER NOT NULL DEFAULT 0,
    info_findings INTEGER NOT NULL DEFAULT 0,
    low_findings INTEGER NOT NULL DEFAULT 0,
    medium_findings INTEGER NOT NULL DEFAULT 0,
    high_findings INTEGER NOT NULL DEFAULT 0,
    critical_findings INTEGER NOT NULL DEFAULT 0,
    unreviewed_findings INTEGER NOT NULL DEFAULT 0,
    suspicious_findings INTEGER NOT NULL DEFAULT 0,
    confirmed_risk_findings INTEGER NOT NULL DEFAULT 0,
    updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
);

CREATE TRIGGER IF NOT EXISTS analysis_v2_runs_insert
AFTER INSERT ON analysis_runs
BEGIN
  INSERT INTO analysis_v2_run_summaries(
      analysis_run_id,artifact_sha256,analyzer_name,analyzer_version,ruleset_version,status)
  VALUES(NEW.id,NEW.artifact_sha256,NEW.analyzer_name,NEW.analyzer_version,NEW.ruleset_version,NEW.status)
  ON CONFLICT(analysis_run_id) DO UPDATE SET
      artifact_sha256=excluded.artifact_sha256,
      analyzer_name=excluded.analyzer_name,
      analyzer_version=excluded.analyzer_version,
      ruleset_version=excluded.ruleset_version,
      status=excluded.status,
      updated_at=CURRENT_TIMESTAMP;
END;

CREATE TRIGGER IF NOT EXISTS analysis_v2_runs_update
AFTER UPDATE OF status,artifact_sha256,analyzer_name,analyzer_version,ruleset_version ON analysis_runs
BEGIN
  UPDATE analysis_v2_run_summaries SET
      artifact_sha256=NEW.artifact_sha256,
      analyzer_name=NEW.analyzer_name,
      analyzer_version=NEW.analyzer_version,
      ruleset_version=NEW.ruleset_version,
      status=NEW.status,
      updated_at=CURRENT_TIMESTAMP
  WHERE analysis_run_id=NEW.id;
END;

CREATE TRIGGER IF NOT EXISTS analysis_v2_files_insert
AFTER INSERT ON analysis_files
BEGIN
  UPDATE analysis_v2_run_summaries SET
      file_count=file_count+1,
      total_file_bytes=total_file_bytes+NEW.byte_size,
      executable_count=executable_count+NEW.executable,
      native_binary_count=native_binary_count+NEW.native_binary,
      generated_count=generated_count+NEW.generated,
      minified_count=minified_count+NEW.minified,
      updated_at=CURRENT_TIMESTAMP
  WHERE analysis_run_id=NEW.analysis_run_id;
END;

CREATE TRIGGER IF NOT EXISTS analysis_v2_dependencies_insert
AFTER INSERT ON analysis_dependencies
BEGIN
  UPDATE analysis_v2_run_summaries SET
      dependency_count=dependency_count+1,
      updated_at=CURRENT_TIMESTAMP
  WHERE analysis_run_id=NEW.analysis_run_id;
END;

CREATE TRIGGER IF NOT EXISTS analysis_v2_findings_insert
AFTER INSERT ON analysis_findings
BEGIN
  INSERT OR IGNORE INTO analysis_v2_rule_definitions(
      ruleset_version,rule_id,category,severity,title,explanation)
  SELECT ruleset_version,NEW.rule_id,NEW.category,NEW.severity,NEW.title,NEW.explanation
  FROM analysis_runs WHERE id=NEW.analysis_run_id;

  SELECT CASE WHEN EXISTS(
      SELECT 1
      FROM analysis_v2_rule_definitions d
      JOIN analysis_runs r ON r.id=NEW.analysis_run_id
      WHERE d.ruleset_version=r.ruleset_version AND d.rule_id=NEW.rule_id
        AND (d.category<>NEW.category OR d.severity<>NEW.severity OR
             d.title<>NEW.title OR d.explanation<>NEW.explanation)
  ) THEN RAISE(ABORT, 'storage v2 rule metadata changed within a ruleset') END;

  INSERT INTO analysis_v2_rule_summaries(
      analysis_run_id,rule_definition_id,occurrence_count)
  SELECT NEW.analysis_run_id,d.id,1
  FROM analysis_v2_rule_definitions d
  JOIN analysis_runs r ON r.id=NEW.analysis_run_id
  WHERE d.ruleset_version=r.ruleset_version AND d.rule_id=NEW.rule_id
  ON CONFLICT(analysis_run_id,rule_definition_id) DO UPDATE SET
      occurrence_count=occurrence_count+1;

  UPDATE analysis_v2_run_summaries SET
      finding_count=finding_count+1,
      info_count=info_count+(NEW.severity='info'),
      low_count=low_count+(NEW.severity='low'),
      medium_count=medium_count+(NEW.severity='medium'),
      high_count=high_count+(NEW.severity='high'),
      critical_count=critical_count+(NEW.severity='critical'),
      unreviewed_count=unreviewed_count+(NEW.disposition='unreviewed'),
      suspicious_count=suspicious_count+(NEW.disposition='suspicious'),
      confirmed_risk_count=confirmed_risk_count+(NEW.disposition='confirmed-risk'),
      updated_at=CURRENT_TIMESTAMP
  WHERE analysis_run_id=NEW.analysis_run_id;
END;

CREATE TRIGGER IF NOT EXISTS analysis_v2_findings_disposition_update
AFTER UPDATE OF disposition ON analysis_findings
WHEN OLD.disposition<>NEW.disposition
BEGIN
  UPDATE analysis_v2_run_summaries SET
      unreviewed_count=unreviewed_count-(OLD.disposition='unreviewed')+(NEW.disposition='unreviewed'),
      suspicious_count=suspicious_count-(OLD.disposition='suspicious')+(NEW.disposition='suspicious'),
      confirmed_risk_count=confirmed_risk_count-(OLD.disposition='confirmed-risk')+(NEW.disposition='confirmed-risk'),
      updated_at=CURRENT_TIMESTAMP
  WHERE analysis_run_id=NEW.analysis_run_id;
END;
"""


@contextmanager
def writer_lock(database: Path) -> Iterator[None]:
    path = Path(str(database) + ".writer.lock")
    descriptor = os.open(path, os.O_RDWR | os.O_CREAT | os.O_CLOEXEC | os.O_NOFOLLOW, 0o600)
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


def require_v1(db: sqlite3.Connection) -> None:
    required = {"analysis_runs", "analysis_files", "analysis_dependencies", "analysis_findings"}
    existing = {
        str(row[0])
        for row in db.execute("SELECT name FROM sqlite_schema WHERE type='table'")
    }
    missing = sorted(required - existing)
    if missing:
        raise RuntimeError("missing v1 analysis tables: " + ", ".join(missing))


def install(db: sqlite3.Connection) -> None:
    require_v1(db)
    db.executescript(DDL)
    db.execute(
        """INSERT INTO storage_v2_info(singleton,schema_version)
           VALUES(1,?)
           ON CONFLICT(singleton) DO UPDATE SET
             schema_version=excluded.schema_version,updated_at=CURRENT_TIMESTAMP""",
        (SCHEMA_VERSION,),
    )


def backfill(db: sqlite3.Connection, batch_size: int) -> int:
    if batch_size < 1 or batch_size > 10_000:
        raise ValueError("batch size must be between 1 and 10000")
    rows = db.execute(
        """SELECT r.id,r.artifact_sha256,r.analyzer_name,r.analyzer_version,
                  r.ruleset_version,r.status
           FROM analysis_runs r
           LEFT JOIN analysis_v2_run_summaries s ON s.analysis_run_id=r.id
           WHERE s.analysis_run_id IS NULL
           ORDER BY r.id LIMIT ?""",
        (batch_size,),
    ).fetchall()
    for row in rows:
        run_id = int(row["id"])
        db.execute(
            """INSERT INTO analysis_v2_run_summaries(
                 analysis_run_id,artifact_sha256,analyzer_name,analyzer_version,
                 ruleset_version,status,file_count,total_file_bytes,dependency_count,
                 finding_count,info_count,low_count,medium_count,high_count,critical_count,
                 executable_count,native_binary_count,generated_count,minified_count,
                 unreviewed_count,suspicious_count,confirmed_risk_count)
               SELECT r.id,r.artifact_sha256,r.analyzer_name,r.analyzer_version,
                      r.ruleset_version,r.status,
                      (SELECT COUNT(*) FROM analysis_files f WHERE f.analysis_run_id=r.id),
                      COALESCE((SELECT SUM(byte_size) FROM analysis_files f WHERE f.analysis_run_id=r.id),0),
                      (SELECT COUNT(*) FROM analysis_dependencies d WHERE d.analysis_run_id=r.id),
                      (SELECT COUNT(*) FROM analysis_findings f WHERE f.analysis_run_id=r.id),
                      (SELECT COUNT(*) FROM analysis_findings f WHERE f.analysis_run_id=r.id AND severity='info'),
                      (SELECT COUNT(*) FROM analysis_findings f WHERE f.analysis_run_id=r.id AND severity='low'),
                      (SELECT COUNT(*) FROM analysis_findings f WHERE f.analysis_run_id=r.id AND severity='medium'),
                      (SELECT COUNT(*) FROM analysis_findings f WHERE f.analysis_run_id=r.id AND severity='high'),
                      (SELECT COUNT(*) FROM analysis_findings f WHERE f.analysis_run_id=r.id AND severity='critical'),
                      (SELECT COUNT(*) FROM analysis_files f WHERE f.analysis_run_id=r.id AND executable=1),
                      (SELECT COUNT(*) FROM analysis_files f WHERE f.analysis_run_id=r.id AND native_binary=1),
                      (SELECT COUNT(*) FROM analysis_files f WHERE f.analysis_run_id=r.id AND generated=1),
                      (SELECT COUNT(*) FROM analysis_files f WHERE f.analysis_run_id=r.id AND minified=1),
                      (SELECT COUNT(*) FROM analysis_findings f WHERE f.analysis_run_id=r.id AND disposition='unreviewed'),
                      (SELECT COUNT(*) FROM analysis_findings f WHERE f.analysis_run_id=r.id AND disposition='suspicious'),
                      (SELECT COUNT(*) FROM analysis_findings f WHERE f.analysis_run_id=r.id AND disposition='confirmed-risk')
               FROM analysis_runs r WHERE r.id=?""",
            (run_id,),
        )
        db.execute(
            """INSERT OR IGNORE INTO analysis_v2_rule_definitions(
                 ruleset_version,rule_id,category,severity,title,explanation)
               SELECT r.ruleset_version,f.rule_id,f.category,f.severity,f.title,f.explanation
               FROM analysis_findings f JOIN analysis_runs r ON r.id=f.analysis_run_id
               WHERE f.analysis_run_id=? GROUP BY f.rule_id""",
            (run_id,),
        )
        mismatch = db.execute(
            """SELECT 1 FROM analysis_findings f
               JOIN analysis_runs r ON r.id=f.analysis_run_id
               JOIN analysis_v2_rule_definitions d
                 ON d.ruleset_version=r.ruleset_version AND d.rule_id=f.rule_id
               WHERE f.analysis_run_id=? AND
                 (d.category<>f.category OR d.severity<>f.severity OR
                  d.title<>f.title OR d.explanation<>f.explanation)
               LIMIT 1""",
            (run_id,),
        ).fetchone()
        if mismatch is not None:
            raise RuntimeError(f"rule metadata mismatch while backfilling analysis run {run_id}")
        db.execute(
            """INSERT INTO analysis_v2_rule_summaries(
                 analysis_run_id,rule_definition_id,occurrence_count)
               SELECT f.analysis_run_id,d.id,COUNT(*)
               FROM analysis_findings f
               JOIN analysis_runs r ON r.id=f.analysis_run_id
               JOIN analysis_v2_rule_definitions d
                 ON d.ruleset_version=r.ruleset_version AND d.rule_id=f.rule_id
               WHERE f.analysis_run_id=?
               GROUP BY f.analysis_run_id,d.id""",
            (run_id,),
        )
    return len(rows)


def refresh_coverage(db: sqlite3.Connection) -> int:
    required = {"static_analysis_schedule_current", "static_analysis_schedule_state"}
    existing = {
        str(row[0])
        for row in db.execute("SELECT name FROM sqlite_schema WHERE type='table'")
    }
    if not required.issubset(existing):
        return 0
    profile = db.execute(
        "SELECT profile_key FROM static_analysis_schedule_current WHERE singleton=1"
    ).fetchone()
    if profile is None:
        return 0
    key = str(profile[0])
    db.execute(
        """INSERT INTO analysis_v2_coverage_summary(
             profile_key,eligible_package_records,completed_package_records,
             failed_package_records,unsupported_package_records,
             unresolvable_package_records,never_attempted_package_records,
             unique_artifacts_analyzed,info_findings,low_findings,medium_findings,
             high_findings,critical_findings,unreviewed_findings,suspicious_findings,
             confirmed_risk_findings)
           SELECT ?,COUNT(*),
             SUM(state='completed'),SUM(state='failed'),SUM(state='unsupported'),
             SUM(state='unresolvable'),SUM(state='eligible' AND attempt_count=0),
             COUNT(DISTINCT CASE WHEN state='completed' THEN artifact_sha256 END),
             COALESCE(SUM(CASE WHEN state='completed' THEN s.info_count END),0),
             COALESCE(SUM(CASE WHEN state='completed' THEN s.low_count END),0),
             COALESCE(SUM(CASE WHEN state='completed' THEN s.medium_count END),0),
             COALESCE(SUM(CASE WHEN state='completed' THEN s.high_count END),0),
             COALESCE(SUM(CASE WHEN state='completed' THEN s.critical_count END),0),
             COALESCE(SUM(CASE WHEN state='completed' THEN s.unreviewed_count END),0),
             COALESCE(SUM(CASE WHEN state='completed' THEN s.suspicious_count END),0),
             COALESCE(SUM(CASE WHEN state='completed' THEN s.confirmed_risk_count END),0)
           FROM static_analysis_schedule_state q
           LEFT JOIN analysis_v2_run_summaries s ON s.analysis_run_id=q.analysis_run_id
           WHERE q.profile_key=?
           ON CONFLICT(profile_key) DO UPDATE SET
             eligible_package_records=excluded.eligible_package_records,
             completed_package_records=excluded.completed_package_records,
             failed_package_records=excluded.failed_package_records,
             unsupported_package_records=excluded.unsupported_package_records,
             unresolvable_package_records=excluded.unresolvable_package_records,
             never_attempted_package_records=excluded.never_attempted_package_records,
             unique_artifacts_analyzed=excluded.unique_artifacts_analyzed,
             info_findings=excluded.info_findings,low_findings=excluded.low_findings,
             medium_findings=excluded.medium_findings,high_findings=excluded.high_findings,
             critical_findings=excluded.critical_findings,
             unreviewed_findings=excluded.unreviewed_findings,
             suspicious_findings=excluded.suspicious_findings,
             confirmed_risk_findings=excluded.confirmed_risk_findings,
             updated_at=CURRENT_TIMESTAMP""",
        (key, key),
    )
    return 1


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--database", required=True, type=Path)
    parser.add_argument("--backfill-batch-size", type=int, default=0)
    parser.add_argument("--refresh-coverage", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    database = args.database.resolve()
    with writer_lock(database):
        db = connect(database)
        try:
            with db:
                install(db)
                processed = backfill(db, args.backfill_batch_size) if args.backfill_batch_size else 0
                coverage = refresh_coverage(db) if args.refresh_coverage else 0
            print(f"storage_v2_schema={SCHEMA_VERSION} backfilled_runs={processed} coverage_rows={coverage}")
        finally:
            db.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
