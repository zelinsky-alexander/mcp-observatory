#!/usr/bin/env python3
from __future__ import annotations

import importlib.util
from pathlib import Path
import sqlite3
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
SPEC = importlib.util.spec_from_file_location(
    "bulk_runtime_discovery_auto", ROOT / "tools" / "bulk_runtime_discovery_auto.py"
)
assert SPEC is not None and SPEC.loader is not None
auto_scheduler = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(auto_scheduler)


def make_db(path: Path) -> sqlite3.Connection:
    db = sqlite3.connect(path)
    db.row_factory = sqlite3.Row
    db.executescript(
        """
        CREATE TABLE server_versions(
          id INTEGER PRIMARY KEY,
          server_identifier TEXT,
          server_version TEXT,
          published_at TEXT
        );
        CREATE TABLE packages(id INTEGER PRIMARY KEY,server_version_id INTEGER,identifier TEXT);
        CREATE TABLE runtime_observation_runs(
          id INTEGER PRIMARY KEY,server_version_id INTEGER,package_id INTEGER,status TEXT,
          artifact_sha256 TEXT,launch_profile_sha256 TEXT,sandbox_image TEXT,guard_version TEXT,
          started_at TEXT,completed_at TEXT
        );
        CREATE TABLE runtime_observation_tools(
          run_id INTEGER,name TEXT,definition_json TEXT,definition_sha256 TEXT
        );
        CREATE TABLE runtime_discovery_schedule_state(
          profile_key TEXT,package_id INTEGER,state TEXT,reason_code TEXT,reason_message TEXT,
          runtime_observation_run_id INTEGER,previous_compatible_run_id INTEGER,
          added_tools INTEGER,removed_tools INTEGER,modified_tools INTEGER,
          unchanged_tools INTEGER,updated_at TEXT
        );
        INSERT INTO server_versions VALUES(1,'io.example/server','1.0.0','2026-01-01T00:00:00Z');
        INSERT INTO server_versions VALUES(2,'io.example/server','2.0.0','2026-02-01T00:00:00Z');
        INSERT INTO server_versions VALUES(3,'io.example/server','3.0.0','2026-03-01T00:00:00Z');
        INSERT INTO packages VALUES(10,1,'example-mcp');
        INSERT INTO packages VALUES(11,2,'example-mcp');
        INSERT INTO packages VALUES(12,3,'example-mcp');
        """
    )
    return db


def profile() -> dict[str, str]:
    return {
        "runtime_image": auto_scheduler.auto.AUTO_RUNTIME_POLICY,
        "guard_sha256": "a" * 64,
    }


def add_run(
    db: sqlite3.Connection,
    run_id: int,
    server_version_id: int,
    package_id: int,
    image: str,
) -> None:
    guard = "sha256:" + "a" * 64
    db.execute(
        "INSERT INTO runtime_observation_runs VALUES(?,?,?,'completed',?,?,?,?,NULL,CURRENT_TIMESTAMP)",
        (
            run_id,
            server_version_id,
            package_id,
            str(run_id)[-1] * 64,
            "b" * 64,
            image,
            guard,
        ),
    )
    db.execute(
        """INSERT INTO runtime_discovery_schedule_state(
             profile_key,package_id,state,runtime_observation_run_id,updated_at)
           VALUES('p',?,'completed',?,CURRENT_TIMESTAMP)""",
        (package_id, run_id),
    )


class AutomaticSchedulerCompatibilityTests(unittest.TestCase):
    def test_auto_profile_accepts_only_approved_images(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            db = make_db(Path(temporary) / "db.sqlite")
            guard = "sha256:" + "a" * 64
            db.execute(
                "INSERT INTO runtime_observation_runs VALUES(101,1,10,'completed',?,?,?,?,NULL,CURRENT_TIMESTAMP)",
                ("1" * 64, "2" * 64, "node:22-trixie-slim", guard),
            )
            db.execute(
                "INSERT INTO runtime_observation_runs VALUES(102,2,11,'completed',?,?,?,?,NULL,CURRENT_TIMESTAMP)",
                ("3" * 64, "4" * 64, "node:22-bookworm-slim", guard),
            )
            db.commit()
            self.assertTrue(auto_scheduler.completed_state_is_valid(db, 101, 10, profile()))
            self.assertFalse(auto_scheduler.completed_state_is_valid(db, 102, 11, profile()))
            db.close()

    def test_drift_compatibility_requires_same_resolved_image_and_earlier_publication(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            db = make_db(Path(temporary) / "db.sqlite")
            add_run(db, 101, 1, 10, "node:22-trixie-slim")
            add_run(db, 102, 2, 11, "node:24-trixie-slim")
            add_run(db, 103, 3, 12, "node:22-trixie-slim")
            db.commit()

            previous = auto_scheduler.previous_compatible_run(
                db, "p", 103, 3, "io.example/server", "example-mcp"
            )
            self.assertEqual(previous, 101)

            previous_for_v2 = auto_scheduler.previous_compatible_run(
                db, "p", 102, 2, "io.example/server", "example-mcp"
            )
            self.assertIsNone(previous_for_v2)
            db.close()

    def test_reconcile_repairs_backward_drift_link(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            db = make_db(Path(temporary) / "db.sqlite")
            add_run(db, 201, 1, 10, "node:22-trixie-slim")
            add_run(db, 202, 3, 12, "node:22-trixie-slim")
            add_run(db, 203, 2, 11, "node:22-trixie-slim")
            for run_id, name in ((201, "old"), (202, "new"), (203, "middle")):
                db.execute(
                    "INSERT INTO runtime_observation_tools VALUES(?,?,?,?)",
                    (run_id, name, '{"name":"' + name + '"}', "f" * 64),
                )
            db.execute(
                """UPDATE runtime_discovery_schedule_state
                   SET previous_compatible_run_id=202,added_tools=1,removed_tools=1,
                       modified_tools=0,unchanged_tools=0
                   WHERE profile_key='p' AND package_id=11"""
            )
            db.commit()

            previous = auto_scheduler.previous_compatible_run(
                db, "p", 203, 2, "io.example/server", "example-mcp"
            )
            self.assertEqual(previous, 201)
            db.close()

    def test_guard_failure_taxonomy(self) -> None:
        samples = {
            "FAIL child exited early": "protocol_child_exited_early",
            "FAIL malformed_json": "protocol_malformed_json",
            "FAIL initialize timeout": "protocol_initialize_timeout",
            "FAIL tools/list timeout": "protocol_tools_list_timeout",
            "FAIL wrong_response_id": "protocol_wrong_response_id",
            "FAIL unsupported_jsonrpc_version": "protocol_unsupported_jsonrpc",
            "FAIL oversized message": "protocol_oversized_message",
            "FAIL unsolicited message": "protocol_unsolicited_message",
        }
        for marker, expected in samples.items():
            stderr = (
                "runtime discovery failed: mcp-native-guard inspect failed: "
                + marker
                + "\ninspect result: failure\n"
            )
            state, code, _ = auto_scheduler.classify_child_failure(stderr, 1)
            self.assertEqual(state, "failed")
            self.assertEqual(code, expected)

        state, code, _ = auto_scheduler.classify_child_failure(
            "runtime discovery failed: mcp-native-guard inspect failed: FAIL mystery\n",
            1,
        )
        self.assertEqual((state, code), ("failed", "protocol_failed_other"))

    def test_reclassifies_existing_generic_protocol_failures(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            db = make_db(Path(temporary) / "db.sqlite")
            db.execute(
                """INSERT INTO runtime_discovery_schedule_state(
                     profile_key,package_id,state,reason_code,reason_message,updated_at)
                   VALUES('p',10,'failed','protocol_failed',?,CURRENT_TIMESTAMP)""",
                (
                    "runtime discovery failed: mcp-native-guard inspect failed: "
                    "FAIL child exited early\ninspect result: failure\n",
                ),
            )
            db.execute(
                """INSERT INTO runtime_discovery_schedule_state(
                     profile_key,package_id,state,reason_code,reason_message,updated_at)
                   VALUES('p',11,'failed','protocol_failed',?,CURRENT_TIMESTAMP)""",
                (
                    "runtime discovery failed: mcp-native-guard inspect failed: "
                    "FAIL malformed_json\ninspect result: failure\n",
                ),
            )
            db.commit()
            changed = auto_scheduler.refine_persisted_protocol_failures(db, "p")
            self.assertEqual(changed, 2)
            rows = {
                int(row["package_id"]): str(row["reason_code"])
                for row in db.execute(
                    "SELECT package_id,reason_code FROM runtime_discovery_schedule_state"
                )
            }
            self.assertEqual(rows[10], "protocol_child_exited_early")
            self.assertEqual(rows[11], "protocol_malformed_json")
            db.close()


if __name__ == "__main__":
    unittest.main()
