#!/usr/bin/env python3
from __future__ import annotations

import importlib.util
from pathlib import Path
import sqlite3
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
SPEC = importlib.util.spec_from_file_location(
    "runtime_discovery_auto", ROOT / "tools" / "runtime_discovery_auto.py"
)
assert SPEC is not None and SPEC.loader is not None
runtime = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runtime)


def make_db(path: Path) -> sqlite3.Connection:
    db = sqlite3.connect(path)
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


class RuntimePrerequisiteTests(unittest.TestCase):
    def test_declared_arguments_are_preserved_in_order(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            db = make_db(Path(temporary) / "db.sqlite")
            db.execute("INSERT INTO package_arguments VALUES(1,1,'second')")
            db.execute("INSERT INTO package_arguments VALUES(1,0,'--first')")
            db.execute("INSERT INTO package_environment VALUES(1,0,'OPTIONAL_TOKEN',0,'optional')")
            arguments, environment = runtime.launch_declarations(db, 1)
            self.assertEqual(arguments, ["--first", "second"])
            self.assertEqual(environment, [{"name": "OPTIONAL_TOKEN", "required": False}])
            db.close()

    def test_required_environment_is_refused_before_launch(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            db = make_db(Path(temporary) / "db.sqlite")
            db.execute("INSERT INTO package_environment VALUES(1,0,'GITHUB_TOKEN',1,'required')")
            with self.assertRaisesRegex(RuntimeError, "required environment unavailable: GITHUB_TOKEN"):
                runtime.launch_declarations(db, 1)
            db.close()

    def test_missing_declared_argument_is_refused(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            db = make_db(Path(temporary) / "db.sqlite")
            db.execute("INSERT INTO package_arguments VALUES(1,0,NULL)")
            with self.assertRaisesRegex(RuntimeError, "declared argument has no value"):
                runtime.launch_declarations(db, 1)
            db.close()

    def test_diagnostic_reader_is_bounded(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "stderr"
            path.write_bytes(b"x" * (runtime.MAX_SERVER_STDERR_BYTES + 100))
            text = runtime._diagnostic_text(path)
            self.assertEqual(len(text.encode("utf-8")), runtime.MAX_SERVER_STDERR_BYTES)


if __name__ == "__main__":
    unittest.main()
