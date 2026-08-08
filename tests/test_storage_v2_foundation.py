#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
from pathlib import Path
import sqlite3
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "storage_v2_foundation", ROOT / "tools" / "storage_v2_foundation.py"
)
assert SPEC is not None and SPEC.loader is not None
storage_v2 = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(storage_v2)

V1_SCHEMA = """
PRAGMA foreign_keys=ON;
CREATE TABLE analysis_runs(
  id INTEGER PRIMARY KEY,
  artifact_sha256 TEXT,
  analyzer_name TEXT NOT NULL,
  analyzer_version TEXT NOT NULL,
  ruleset_version TEXT NOT NULL,
  status TEXT NOT NULL
);
CREATE TABLE analysis_files(
  id INTEGER PRIMARY KEY,
  analysis_run_id INTEGER NOT NULL REFERENCES analysis_runs(id) ON DELETE CASCADE,
  archive_path TEXT NOT NULL,
  file_type TEXT NOT NULL,
  byte_size INTEGER NOT NULL,
  sha256 TEXT NOT NULL,
  executable INTEGER NOT NULL,
  native_binary INTEGER NOT NULL,
  generated INTEGER NOT NULL,
  minified INTEGER NOT NULL,
  UNIQUE(analysis_run_id, archive_path)
);
CREATE TABLE analysis_dependencies(
  id INTEGER PRIMARY KEY,
  analysis_run_id INTEGER NOT NULL REFERENCES analysis_runs(id) ON DELETE CASCADE,
  dependency_type TEXT NOT NULL,
  dependency_name TEXT NOT NULL,
  declared_version TEXT NOT NULL,
  resolved_version TEXT,
  direct INTEGER NOT NULL,
  development INTEGER NOT NULL
);
CREATE TABLE analysis_findings(
  id INTEGER PRIMARY KEY,
  analysis_run_id INTEGER NOT NULL REFERENCES analysis_runs(id) ON DELETE CASCADE,
  rule_id TEXT NOT NULL,
  category TEXT NOT NULL,
  severity TEXT NOT NULL,
  confidence TEXT NOT NULL,
  disposition TEXT NOT NULL,
  subject_path TEXT NOT NULL,
  line_number INTEGER,
  symbol TEXT,
  title TEXT NOT NULL,
  evidence TEXT,
  explanation TEXT NOT NULL
);
"""


def add_run(db: sqlite3.Connection, run_id: int, ruleset: str = "rules-1") -> None:
    db.execute(
        "INSERT INTO analysis_runs VALUES(?,?,?,?,?,?)",
        (run_id, f"sha-{run_id}", "static", "1.1.0", ruleset, "completed"),
    )


def add_finding(
    db: sqlite3.Connection,
    run_id: int,
    rule_id: str = "risk-api:fetch",
    severity: str = "medium",
    disposition: str = "unreviewed",
    title: str = "Fetch use",
    explanation: str = "Network-capable API use",
) -> None:
    db.execute(
        """INSERT INTO analysis_findings(
             analysis_run_id,rule_id,category,severity,confidence,disposition,
             subject_path,line_number,symbol,title,evidence,explanation)
           VALUES(?,?,?,?,?,?,?,?,?,?,?,?)""",
        (
            run_id,
            rule_id,
            "risk-api",
            severity,
            "high",
            disposition,
            "index.js",
            7,
            "fetch",
            title,
            "fetch(url)",
            explanation,
        ),
    )


class StorageV2FoundationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.database = Path(self.temp.name) / "catalog.sqlite"
        self.db = sqlite3.connect(self.database)
        self.db.row_factory = sqlite3.Row
        self.db.executescript(V1_SCHEMA)

    def tearDown(self) -> None:
        self.db.close()
        self.temp.cleanup()

    def test_install_dual_writes_compact_summaries(self) -> None:
        storage_v2.install(self.db)
        add_run(self.db, 1)
        self.db.execute(
            """INSERT INTO analysis_files(
                 analysis_run_id,archive_path,file_type,byte_size,sha256,
                 executable,native_binary,generated,minified)
               VALUES(1,'bin/tool','script',100,'file-sha',1,0,1,0)"""
        )
        self.db.execute(
            """INSERT INTO analysis_dependencies(
                 analysis_run_id,dependency_type,dependency_name,declared_version,
                 resolved_version,direct,development)
               VALUES(1,'runtime','dep','^1','1.2.3',1,0)"""
        )
        add_finding(self.db, 1)
        add_finding(self.db, 1)

        summary = self.db.execute(
            "SELECT * FROM analysis_v2_run_summaries WHERE analysis_run_id=1"
        ).fetchone()
        self.assertEqual(summary["file_count"], 1)
        self.assertEqual(summary["total_file_bytes"], 100)
        self.assertEqual(summary["dependency_count"], 1)
        self.assertEqual(summary["finding_count"], 2)
        self.assertEqual(summary["medium_count"], 2)
        self.assertEqual(summary["unreviewed_count"], 2)
        self.assertEqual(summary["executable_count"], 1)

        definitions = self.db.execute(
            "SELECT COUNT(*) FROM analysis_v2_rule_definitions"
        ).fetchone()[0]
        rule_summary = self.db.execute(
            "SELECT occurrence_count FROM analysis_v2_rule_summaries"
        ).fetchone()[0]
        self.assertEqual(definitions, 1)
        self.assertEqual(rule_summary, 2)

    def test_rule_metadata_change_within_ruleset_is_rejected(self) -> None:
        storage_v2.install(self.db)
        add_run(self.db, 1)
        add_finding(self.db, 1)
        with self.assertRaises(sqlite3.IntegrityError):
            add_finding(self.db, 1, severity="high")

    def test_disposition_update_updates_summary(self) -> None:
        storage_v2.install(self.db)
        add_run(self.db, 1)
        add_finding(self.db, 1)
        finding_id = self.db.execute("SELECT id FROM analysis_findings").fetchone()[0]
        self.db.execute(
            "UPDATE analysis_findings SET disposition='suspicious' WHERE id=?",
            (finding_id,),
        )
        summary = self.db.execute(
            "SELECT unreviewed_count,suspicious_count FROM analysis_v2_run_summaries"
        ).fetchone()
        self.assertEqual(tuple(summary), (0, 1))

    def test_backfill_existing_v1_run(self) -> None:
        add_run(self.db, 1)
        self.db.execute(
            """INSERT INTO analysis_files(
                 analysis_run_id,archive_path,file_type,byte_size,sha256,
                 executable,native_binary,generated,minified)
               VALUES(1,'index.js','javascript',123,'f',0,0,0,1)"""
        )
        add_finding(self.db, 1, severity="high", disposition="suspicious")
        storage_v2.install(self.db)

        processed = storage_v2.backfill(self.db, 100)
        self.assertEqual(processed, 1)
        summary = self.db.execute(
            "SELECT * FROM analysis_v2_run_summaries WHERE analysis_run_id=1"
        ).fetchone()
        self.assertEqual(summary["file_count"], 1)
        self.assertEqual(summary["high_count"], 1)
        self.assertEqual(summary["suspicious_count"], 1)
        self.assertEqual(summary["minified_count"], 1)
        self.assertEqual(
            self.db.execute("SELECT COUNT(*) FROM analysis_v2_rule_definitions").fetchone()[0],
            1,
        )

    def test_evidence_manifest_is_locator_only(self) -> None:
        storage_v2.install(self.db)
        add_run(self.db, 1)
        self.db.execute(
            """INSERT INTO analysis_v2_evidence_manifests(
                 analysis_run_id,storage_kind,locator,bundle_sha256,inventory_sha256,
                 retained_artifact)
               VALUES(1,'legacy-directory','sha256/aa/example',NULL,'inventory-sha',0)"""
        )
        row = self.db.execute(
            "SELECT storage_kind,locator,retained_artifact FROM analysis_v2_evidence_manifests"
        ).fetchone()
        self.assertEqual(tuple(row), ("legacy-directory", "sha256/aa/example", 0))


if __name__ == "__main__":
    unittest.main()
