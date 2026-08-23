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

    def classify(self, server_stderr: str) -> tuple[str, str, str]:
        wrapped = (
            "Traceback (most recent call last):\n"
            "  File '/tmp/wrapper.py', line 1, in <module>\n"
            "RuntimeError: mcp-native-guard inspect failed: "
            "FAIL child exited early inspect result: failure\n"
            "server stderr: " + server_stderr
        )
        return scheduler.classify_child_failure(wrapped, 2)

    def test_compacts_python_traceback(self) -> None:
        state, code, message = self.classify("Unknown argument: .. Run arkgate --help for usage.")
        self.assertEqual((state, code), ("blocked", "blocked_startup_arguments"))
        self.assertNotIn("Traceback", message)
        self.assertTrue(message.startswith("mcp-native-guard inspect failed:"))
        self.assertIn("server stderr: Unknown argument", message)

    def test_classifies_network_prerequisite(self) -> None:
        state, code, _ = self.classify(
            "TypeError: fetch failed; getaddrinfo EAI_AGAIN api.negirau.com"
        )
        self.assertEqual((state, code), ("blocked", "blocked_startup_network"))

    def test_classifies_writable_path_prerequisite(self) -> None:
        state, code, _ = self.classify(
            "Fatal error: ENOENT: no such file or directory, mkdir '/.cogmemory'"
        )
        self.assertEqual((state, code), ("blocked", "blocked_startup_writable_path"))

    def test_classifies_workspace_prerequisite(self) -> None:
        state, code, _ = self.classify(
            '{"code":"AUTO_MOD_ROOT_NOT_FOUND","message":"Start the MCP from inside a Hearts of Iron IV mod or configure a server workspace"}'
        )
        self.assertEqual((state, code), ("blocked", "blocked_startup_workspace"))

    def test_classifies_external_binary_prerequisite(self) -> None:
        state, code, _ = self.classify(
            "Could not find the Nimbus CLI. Install it, or set NIMBUS_BIN to its full path."
        )
        self.assertEqual((state, code), ("blocked", "blocked_startup_external_binary"))

    def test_missing_packaged_module_is_inconclusive_package_error(self) -> None:
        state, code, _ = self.classify(
            "Error [ERR_MODULE_NOT_FOUND]: Cannot find module '/work/node_modules/aiterm-mcp/dist/vendors/grok.js'"
        )
        self.assertEqual(
            (state, code),
            ("inconclusive", "inconclusive_startup_package_error"),
        )

    def test_protocol_failure_remains_failed_but_compact(self) -> None:
        wrapped = (
            "Traceback (most recent call last):\n"
            "RuntimeError: mcp-native-guard inspect failed: "
            "FAIL malformed_json inspect result: failure"
        )
        state, code, message = scheduler.classify_child_failure(wrapped, 2)
        self.assertEqual((state, code), ("failed", "protocol_malformed_json"))
        self.assertNotIn("Traceback", message)
        self.assertEqual(
            message,
            "mcp-native-guard inspect failed: FAIL malformed_json inspect result: failure",
        )


if __name__ == "__main__":
    unittest.main()
