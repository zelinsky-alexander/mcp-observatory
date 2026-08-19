from __future__ import annotations

import importlib.util
from pathlib import Path
import sqlite3
import tempfile
import unittest

TOOL_PATH = Path(__file__).resolve().parents[1] / "tools" / "storage_v2_portal_history_indexes.py"
SPEC = importlib.util.spec_from_file_location("storage_v2_portal_history_indexes", TOOL_PATH)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class StorageV2PortalHistoryIndexTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.database = Path(self.temporary.name) / "history.sqlite"
        db = sqlite3.connect(self.database)
        try:
            db.executescript(
                """
                CREATE TABLE static_analysis_schedule_state(
                    profile_key TEXT NOT NULL,
                    package_id INTEGER NOT NULL,
                    state TEXT NOT NULL,
                    attempt_count INTEGER NOT NULL,
                    updated_at TEXT,
                    PRIMARY KEY(profile_key, package_id)
                );
                INSERT INTO static_analysis_schedule_state VALUES
                    ('profile',101,'completed',1,'2026-05-01T00:00:00Z'),
                    ('profile',102,'eligible',0,'2026-05-03T00:00:00Z'),
                    ('profile',103,'failed',2,'2026-05-02T00:00:00Z'),
                    ('profile',104,'unsupported',0,'2026-05-04T00:00:00Z'),
                    ('other',105,'completed',1,'2026-06-01T00:00:00Z');
                """
            )
            db.commit()
        finally:
            db.close()

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_install_is_idempotent_and_plans_use_expected_indexes(self) -> None:
        MODULE.install(self.database)
        MODULE.install(self.database)
        result = MODULE.status(self.database)
        self.assertTrue(all(result["installed"].values()))
        self.assertTrue(all(result["uses_expected_index"].values()))

        db = sqlite3.connect(self.database)
        try:
            completed = [row[0] for row in db.execute(MODULE.COVERAGE_COMPLETED_SQL)]
            never = [row[0] for row in db.execute(MODULE.COVERAGE_NEVER_SQL)]
            eligible = [row[0] for row in db.execute(MODULE.COVERAGE_ELIGIBLE_SQL)]
        finally:
            db.close()

        self.assertEqual(completed, [101])
        self.assertEqual(never, [102])
        self.assertEqual(eligible, [102, 103, 101])


if __name__ == "__main__":
    unittest.main()
