from __future__ import annotations

import importlib.util
from pathlib import Path
import sqlite3
import tempfile
import unittest


TOOL_PATH = Path(__file__).resolve().parents[1] / "tools" / "storage_v2_review_queue_index.py"
SPEC = importlib.util.spec_from_file_location("storage_v2_review_queue_index", TOOL_PATH)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class StorageV2ReviewQueueIndexTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.database = Path(self.temporary.name) / "history.sqlite"
        db = sqlite3.connect(self.database)
        try:
            db.executescript(
                """
                CREATE TABLE analysis_findings(
                    id INTEGER PRIMARY KEY,
                    analysis_run_id INTEGER NOT NULL,
                    severity TEXT NOT NULL,
                    disposition TEXT NOT NULL
                );
                INSERT INTO analysis_findings VALUES
                    (1, 10, 'low', 'unreviewed'),
                    (2, 11, 'high', 'unreviewed'),
                    (3, 12, 'critical', 'unreviewed'),
                    (4, 13, 'high', 'accepted-risk'),
                    (5, 14, 'high', 'unreviewed');
                """
            )
            db.commit()
        finally:
            db.close()

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_install_is_idempotent_and_query_plan_uses_partial_index(self) -> None:
        MODULE.install(self.database)
        MODULE.install(self.database)

        result = MODULE.status(self.database)
        self.assertTrue(result["installed"])
        self.assertTrue(result["uses_index"])

        db = sqlite3.connect(self.database)
        try:
            ids = [
                int(row[0])
                for row in db.execute(MODULE.PAGE_SQL, (50, 0)).fetchall()
            ]
        finally:
            db.close()

        # BINARY severity order puts critical before high; within severity the
        # newest analysis run/finding comes first.
        self.assertEqual(ids, [3, 5, 2])


if __name__ == "__main__":
    unittest.main()
