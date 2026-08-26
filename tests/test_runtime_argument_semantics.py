#!/usr/bin/env python3
from __future__ import annotations

import importlib.util
import json
import os
from pathlib import Path
import sqlite3
import sys
import tempfile
import unittest
from unittest import mock

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
        CREATE TABLE server_versions(
          id INTEGER PRIMARY KEY,server_identifier TEXT,server_version TEXT
        );
        CREATE TABLE packages(
          id INTEGER PRIMARY KEY,server_version_id INTEGER,registry_type TEXT,
          identifier TEXT,version TEXT,transport TEXT
        );
        CREATE TABLE package_arguments(
          package_id INTEGER,position INTEGER,argument_value TEXT,
          PRIMARY KEY(package_id,position)
        );
        CREATE TABLE package_environment(
          package_id INTEGER,position INTEGER,name TEXT,required INTEGER,description TEXT,
          PRIMARY KEY(package_id,position)
        );
        CREATE TABLE runtime_discovery_schedule_state(
          profile_key TEXT NOT NULL,
          package_id INTEGER NOT NULL,
          state TEXT NOT NULL,
          reason_code TEXT,
          reason_message TEXT,
          attempt_count INTEGER NOT NULL DEFAULT 0,
          runtime_observation_run_id INTEGER,
          artifact_sha256 TEXT,
          launch_profile_sha256 TEXT,
          previous_compatible_run_id INTEGER,
          added_tools INTEGER,
          removed_tools INTEGER,
          modified_tools INTEGER,
          unchanged_tools INTEGER,
          discovered_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
          last_attempt_at TEXT,
          updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
          PRIMARY KEY(profile_key,package_id)
        );
        """
    )
    return db


def seed_runtime_state(
    db: sqlite3.Connection,
    state: str,
    reason_code: str,
    reason_message: str,
    *,
    transport: str = "stdio",
) -> None:
    db.execute("INSERT INTO server_versions VALUES(1,'example/server','1.0.0')")
    db.execute(
        "INSERT INTO packages VALUES(7,1,'npm','example-mcp','1.0.0',?)",
        (transport,),
    )
    db.execute(
        """INSERT INTO runtime_discovery_schedule_state(
             profile_key,package_id,state,reason_code,reason_message,attempt_count)
           VALUES('profile',7,?,?,?,1)""",
        (state, reason_code, reason_message),
    )


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

    def test_runtime_child_forwards_only_explicit_tmpdir(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            with mock.patch.dict(
                os.environ,
                {"TMPDIR": temporary, "MCPO_SHOULD_NOT_LEAK": "secret"},
                clear=False,
            ):
                result = scheduler.run_child(
                    [
                        sys.executable,
                        "-c",
                        (
                            "import json,os;"
                            "print(json.dumps({'tmpdir':os.environ.get('TMPDIR'),"
                            "'leak':os.environ.get('MCPO_SHOULD_NOT_LEAK')}))"
                        ),
                    ],
                    timeout=10,
                    output_limit=4096,
                )
            self.assertEqual(result.returncode, 0)
            payload = json.loads(result.stdout.decode("utf-8"))
            self.assertEqual(payload["tmpdir"], temporary)
            self.assertIsNone(payload["leak"])

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

    def test_terminal_runtime_outcomes_survive_synchronize_and_are_not_candidates(self) -> None:
        cases = (
            (
                "blocked",
                "blocked_startup_network",
                "mcp-native-guard inspect failed: FAIL child exited early\n"
                "server stderr: getaddrinfo EAI_AGAIN api.example.com",
            ),
            (
                "inconclusive",
                "inconclusive_startup_package_error",
                "mcp-native-guard inspect failed: FAIL child exited early\n"
                "server stderr: Error [ERR_MODULE_NOT_FOUND]",
            ),
        )
        for state, code, message in cases:
            with self.subTest(state=state):
                with tempfile.TemporaryDirectory() as temporary:
                    db = make_db(Path(temporary) / "db.sqlite")
                    seed_runtime_state(db, state, code, message)

                    scheduler.preserve_terminal_outcomes_during_sync(
                        db, "profile", {}, 7200
                    )

                    row = db.execute(
                        """SELECT state,reason_code,reason_message
                           FROM runtime_discovery_schedule_state
                           WHERE profile_key='profile' AND package_id=7"""
                    ).fetchone()
                    self.assertIsNotNone(row)
                    assert row is not None
                    self.assertEqual(row["state"], state)
                    self.assertEqual(row["reason_code"], code)
                    self.assertEqual(row["reason_message"], message)
                    self.assertEqual(
                        scheduler.scheduler.base.candidates(
                            db, "profile", 10, 3, 0
                        ),
                        [],
                    )
                    db.close()

    def test_real_eligibility_change_replaces_terminal_runtime_outcome(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            db = make_db(Path(temporary) / "db.sqlite")
            seed_runtime_state(
                db,
                "blocked",
                "blocked_startup_network",
                "network prerequisite",
                transport="streamable-http",
            )

            scheduler.preserve_terminal_outcomes_during_sync(db, "profile", {}, 7200)

            row = db.execute(
                """SELECT state,reason_code FROM runtime_discovery_schedule_state
                   WHERE profile_key='profile' AND package_id=7"""
            ).fetchone()
            self.assertIsNotNone(row)
            assert row is not None
            self.assertEqual(row["state"], "unsupported")
            self.assertEqual(row["reason_code"], "unsupported_transport")
            db.close()


if __name__ == "__main__":
    unittest.main()
