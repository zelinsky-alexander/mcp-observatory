#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
from pathlib import Path
import sqlite3
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "storage_v2_registry_sync", ROOT / "tools" / "storage_v2_registry_sync.py"
)
assert SPEC is not None and SPEC.loader is not None
syncer = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(syncer)

SCHEMA = """
CREATE TABLE snapshots(id INTEGER PRIMARY KEY,snapshot_sha256 TEXT,completed_at TEXT);
CREATE TABLE server_versions(id INTEGER PRIMARY KEY,server_identifier TEXT,server_version TEXT);
CREATE TABLE snapshot_server_versions(snapshot_id INTEGER,server_version_id INTEGER,PRIMARY KEY(snapshot_id,server_version_id));
CREATE TABLE repositories(server_version_id INTEGER PRIMARY KEY,url TEXT);
CREATE TABLE packages(id INTEGER PRIMARY KEY,server_version_id INTEGER,position INTEGER,registry_type TEXT,identifier TEXT,version TEXT,transport TEXT);
CREATE TABLE package_arguments(package_id INTEGER,position INTEGER,argument_value TEXT,PRIMARY KEY(package_id,position));
CREATE TABLE package_environment(package_id INTEGER,position INTEGER,name TEXT,required INTEGER,description TEXT,PRIMARY KEY(package_id,position));
CREATE TABLE remotes(id INTEGER PRIMARY KEY,server_version_id INTEGER,position INTEGER,url TEXT,transport TEXT);
"""


class RegistrySyncTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.hot = Path(self.temp.name) / "hot.sqlite"
        self.history = Path(self.temp.name) / "history.sqlite"
        for path in (self.hot, self.history):
            db = sqlite3.connect(path)
            db.executescript(SCHEMA)
            db.execute("INSERT INTO snapshots VALUES(1,'old','2026-08-06')")
            db.execute("INSERT INTO server_versions VALUES(1,'server','1.0')")
            db.execute("INSERT INTO snapshot_server_versions VALUES(1,1)")
            db.execute("INSERT INTO repositories VALUES(1,'https://example.test/repo')")
            db.execute("INSERT INTO packages VALUES(1,1,0,'npm','pkg','1.0','stdio')")
            db.commit()
            db.close()
        hot = sqlite3.connect(self.hot)
        hot.execute("INSERT INTO snapshots VALUES(2,'new','2026-08-07')")
        hot.execute("INSERT INTO server_versions VALUES(2,'server','1.1')")
        hot.execute("INSERT INTO snapshot_server_versions VALUES(2,2)")
        hot.execute("INSERT INTO repositories VALUES(2,'https://example.test/repo')")
        hot.execute("INSERT INTO packages VALUES(2,2,0,'npm','pkg','1.1','stdio')")
        hot.commit()
        hot.close()

    def tearDown(self) -> None:
        self.temp.cleanup()

    def test_sync_appends_new_registry_rows_without_replacing_existing(self) -> None:
        result = syncer.sync(self.hot, self.history)
        self.assertEqual(result["snapshots"]["inserted"], 1)
        self.assertEqual(result["server_versions"]["inserted"], 1)
        self.assertEqual(result["packages"]["inserted"], 1)
        db = sqlite3.connect(self.history)
        self.assertEqual(db.execute("SELECT COUNT(*) FROM snapshots").fetchone()[0], 2)
        self.assertEqual(db.execute("SELECT COUNT(*) FROM packages").fetchone()[0], 2)
        db.close()

    def test_sync_rejects_mutation_of_existing_immutable_row(self) -> None:
        hot = sqlite3.connect(self.hot)
        hot.execute("UPDATE packages SET identifier='changed' WHERE id=1")
        hot.commit()
        hot.close()
        with self.assertRaises(RuntimeError):
            syncer.sync(self.hot, self.history)


if __name__ == "__main__":
    unittest.main()
