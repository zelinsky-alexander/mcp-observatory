#!/usr/bin/env python3
from __future__ import annotations

import importlib.util
from pathlib import Path
import sqlite3
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent


def load(name: str, filename: str):
    spec = importlib.util.spec_from_file_location(name, ROOT / "tools" / filename)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


runtime = load("runtime_argument_semantics", "runtime_discovery_argument_semantics.py")
scheduler = load("bulk_argument_semantics", "bulk_runtime_discovery_argument_semantics.py")


def make_db(path: Path) -> sqlite3.Connection:
    db = sqlite3.connect(path)
    db.row_factory = sqlite3.Row
    db.executescript(
        """
        CREATE TABLE package_arguments(
          package_id INTEGER,position INTEGER,argument_value TEXT,
          PRIMARY KEY(package_id,position)
        );
        CREATE TABLE package_environment(
          package_id INTEGER,position INTEGER,name TEXT,required INTEGER,description TEXT,
          PRIMARY KEY(package_id,position)
        );
        """
    )
    return db


class RuntimeArgumentSemanticsTests(unittest.TestCase):
    def test_null_argument_is_not_preblocked(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            db = make_db(Path(temporary) / "db.sqlite")
            db.execute("INSERT INTO package_arguments VALUES(7,0,NULL)")
            self.assertIsNone(scheduler.package_prerequisite(db, 7))
            arguments, environment = runtime.launch_declarations(db, 7)
            self.assertEqual(arguments, [])
            self.assertEqual(environment, [])
            db.close()

    def test_concrete_arguments_remain_ordered(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            db = make_db(Path(temporary) / "db.sqlite")
            db.execute("INSERT INTO package_arguments VALUES(7,2,'third')")
            db.execute("INSERT INTO package_arguments VALUES(7,0,'--first')")
            db.execute("INSERT INTO package_arguments VALUES(7,1,NULL)")
            arguments, _ = runtime.launch_declarations(db, 7)
            self.assertEqual(arguments, ["--first", "third"])
            db.close()

    def test_required_environment_remains_blocked(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            db = make_db(Path(temporary) / "db.sqlite")
            db.execute("INSERT INTO package_environment VALUES(7,0,'API_KEY',1,'required')")
            outcome = scheduler.package_prerequisite(db, 7)
            self.assertIsNotNone(outcome)
            assert outcome is not None
            self.assertEqual(outcome[:2], ("blocked", "blocked_required_environment"))
            with self.assertRaisesRegex(RuntimeError, "required environment unavailable: API_KEY"):
                runtime.launch_declarations(db, 7)
            db.close()


if __name__ == "__main__":
    unittest.main()
