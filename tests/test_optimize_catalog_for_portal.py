from __future__ import annotations

import importlib.util
from pathlib import Path
import sqlite3
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "optimize_catalog_for_portal.py"
SPEC = importlib.util.spec_from_file_location("optimize_catalog_for_portal", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class OptimizeCatalogForPortalTests(unittest.TestCase):
    def test_creates_only_indexes_for_existing_tables(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            database = Path(temporary) / "catalog.sqlite"
            connection = sqlite3.connect(database)
            connection.executescript(
                """
                CREATE TABLE analysis_runs(
                    id INTEGER PRIMARY KEY,
                    status TEXT NOT NULL,
                    started_at TEXT NOT NULL
                );
                CREATE TABLE analysis_findings(
                    id INTEGER PRIMARY KEY,
                    analysis_run_id INTEGER NOT NULL,
                    disposition TEXT NOT NULL,
                    severity TEXT NOT NULL
                );
                """
            )
            connection.commit()
            connection.close()

            MODULE.optimize(database, 5_000)

            connection = sqlite3.connect(database)
            indexes = {
                row[0]
                for row in connection.execute(
                    "SELECT name FROM sqlite_schema WHERE type='index'"
                )
                if row[0] is not None
            }
            connection.close()

            self.assertIn("portal_analysis_runs_status", indexes)
            self.assertIn("portal_analysis_runs_started", indexes)
            self.assertIn(
                "portal_analysis_findings_disposition_severity", indexes
            )
            self.assertNotIn("portal_static_schedule_coverage", indexes)


if __name__ == "__main__":
    unittest.main()
