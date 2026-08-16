from __future__ import annotations

import importlib.util
from pathlib import Path
import sqlite3
import tempfile
import unittest


TOOL_PATH = Path(__file__).resolve().parents[1] / "tools" / "storage_v2_portal_hot_indexes.py"
SPEC = importlib.util.spec_from_file_location("storage_v2_portal_hot_indexes", TOOL_PATH)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class StorageV2PortalHotIndexTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.database = Path(self.temporary.name) / "hot.sqlite"
        db = sqlite3.connect(self.database)
        try:
            db.executescript(
                """
                CREATE TABLE server_versions(
                    id INTEGER PRIMARY KEY,
                    server_identifier TEXT NOT NULL,
                    updated_at TEXT,
                    published_at TEXT
                );
                CREATE TABLE analysis_runs(
                    id INTEGER PRIMARY KEY,
                    status TEXT NOT NULL,
                    started_at TEXT NOT NULL
                );
                INSERT INTO server_versions VALUES
                    (1,'a','2026-01-01T00:00:00Z','2025-01-01T00:00:00Z'),
                    (2,'a','2026-02-01T00:00:00Z','2025-02-01T00:00:00Z'),
                    (3,'b',NULL,'2026-03-01T00:00:00Z');
                INSERT INTO analysis_runs VALUES
                    (10,'failed','2026-01-01T00:00:00Z'),
                    (11,'completed','2026-02-01T00:00:00Z'),
                    (12,'completed','2026-03-01T00:00:00Z');
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
            records = [row[0] for row in db.execute(MODULE.RECORDS_SQL)]
            analyses = [row[0] for row in db.execute(MODULE.ANALYSES_SQL)]
            servers = [row[0] for row in db.execute(MODULE.ALL_SERVERS_SQL)]
        finally:
            db.close()

        self.assertEqual(records, [3, 2, 1])
        self.assertEqual(analyses, [12, 11])
        self.assertEqual(servers, [3, 2])


if __name__ == "__main__":
    unittest.main()
