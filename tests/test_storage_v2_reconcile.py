from __future__ import annotations

import importlib.util
from pathlib import Path
import sqlite3
import tempfile
import unittest


TOOL = Path(__file__).resolve().parents[1] / "tools" / "storage_v2_reconcile.py"
SPEC = importlib.util.spec_from_file_location("storage_v2_reconcile", TOOL)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


SCHEMA = """
CREATE TABLE static_analysis_schedule_current(
  singleton INTEGER PRIMARY KEY,
  profile_key TEXT NOT NULL
);
CREATE TABLE static_analysis_schedule_state(
  profile_key TEXT NOT NULL,
  package_id INTEGER NOT NULL,
  state TEXT NOT NULL,
  attempt_count INTEGER NOT NULL,
  analysis_run_id INTEGER,
  artifact_sha256 TEXT
);
CREATE TABLE analysis_v2_run_summaries(
  analysis_run_id INTEGER PRIMARY KEY,
  info_count INTEGER NOT NULL,
  low_count INTEGER NOT NULL,
  medium_count INTEGER NOT NULL,
  high_count INTEGER NOT NULL,
  critical_count INTEGER NOT NULL,
  unreviewed_count INTEGER NOT NULL,
  unreviewed_high_count INTEGER NOT NULL,
  unreviewed_critical_count INTEGER NOT NULL,
  suspicious_count INTEGER NOT NULL,
  confirmed_risk_count INTEGER NOT NULL
);
CREATE TABLE analysis_v2_coverage_summary(
  profile_key TEXT PRIMARY KEY,
  eligible_package_records INTEGER NOT NULL DEFAULT 0,
  completed_package_records INTEGER NOT NULL DEFAULT 0,
  failed_package_records INTEGER NOT NULL DEFAULT 0,
  unsupported_package_records INTEGER NOT NULL DEFAULT 0,
  unresolvable_package_records INTEGER NOT NULL DEFAULT 0,
  never_attempted_package_records INTEGER NOT NULL DEFAULT 0,
  running_package_records INTEGER NOT NULL DEFAULT 0,
  unique_artifacts_analyzed INTEGER NOT NULL DEFAULT 0,
  info_findings INTEGER NOT NULL DEFAULT 0,
  low_findings INTEGER NOT NULL DEFAULT 0,
  medium_findings INTEGER NOT NULL DEFAULT 0,
  high_findings INTEGER NOT NULL DEFAULT 0,
  critical_findings INTEGER NOT NULL DEFAULT 0,
  unreviewed_findings INTEGER NOT NULL DEFAULT 0,
  unreviewed_high_or_critical_findings INTEGER NOT NULL DEFAULT 0,
  suspicious_findings INTEGER NOT NULL DEFAULT 0,
  confirmed_risk_findings INTEGER NOT NULL DEFAULT 0,
  updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
);
"""


class ReconcileCoverageTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.path = Path(self.tmp.name) / "catalog.sqlite"
        db = sqlite3.connect(self.path)
        db.executescript(SCHEMA)
        db.execute("INSERT INTO static_analysis_schedule_current VALUES(1,'profile')")
        # Two package records deliberately reuse one completed canonical run.
        db.executemany(
            "INSERT INTO static_analysis_schedule_state VALUES(?,?,?,?,?,?)",
            [
                ("profile", 10, "completed", 1, 100, "a" * 64),
                ("profile", 20, "completed", 1, 100, "a" * 64),
                ("profile", 30, "failed", 3, None, None),
                ("profile", 40, "unsupported", 0, None, None),
            ],
        )
        db.execute(
            """INSERT INTO analysis_v2_run_summaries VALUES(
                 100,1,2,3,7,0,10,7,0,1,0)"""
        )
        db.commit()
        db.close()

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def test_reused_run_findings_are_counted_once(self) -> None:
        db = MODULE.connect(self.path)
        try:
            with db:
                result = MODULE.reconcile(db)
            row = db.execute(
                "SELECT * FROM analysis_v2_coverage_summary WHERE profile_key='profile'"
            ).fetchone()
        finally:
            db.close()
        self.assertEqual(result["completed_package_records"], 2)
        self.assertEqual(result["unique_artifacts_analyzed"], 1)
        self.assertEqual(result["distinct_completed_analysis_runs"], 1)
        self.assertEqual(result["finding_occurrences"], 13)
        self.assertEqual(int(row["high_findings"]), 7)
        self.assertEqual(int(row["unreviewed_high_or_critical_findings"]), 7)
        self.assertEqual(int(row["failed_package_records"]), 1)
        self.assertEqual(int(row["unsupported_package_records"]), 1)


if __name__ == "__main__":
    unittest.main()
