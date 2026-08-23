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
        CREATE TABLE server_versions(id INTEGER PRIMARY KEY,server_identifier TEXT,server_version TEXT);
        CREATE TABLE packages(id INTEGER PRIMARY KEY,server_version_id INTEGER,identifier TEXT);
        CREATE TABLE runtime_observation_runs(
          id INTEGER PRIMARY KEY,server_version_id INTEGER,package_id INTEGER,status TEXT,
          artifact_sha256 TEXT,launch_profile_sha256 TEXT,sandbox_image TEXT,guard_version TEXT,
          started_at TEXT,completed_at TEXT
        );
        CREATE TABLE runtime_discovery_schedule_state(
          profile_key TEXT,package_id INTEGER,state TEXT,runtime_observation_run_id INTEGER
        );
        INSERT INTO server_versions VALUES(1,'io.example/server','1.0.0');
        INSERT INTO server_versions VALUES(2,'io.example/server','2.0.0');
        INSERT INTO server_versions VALUES(3,'io.example/server','3.0.0');
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

    def test_drift_compatibility_requires_same_resolved_image(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            db = make_db(Path(temporary) / "db.sqlite")
            guard = "sha256:" + "a" * 64
            rows = [
                (101,1,10,"node:22-trixie-slim"),
                (102,2,11,"node:24-trixie-slim"),
                (103,3,12,"node:22-trixie-slim"),
            ]
            for run_id, sv_id, pkg_id, image in rows:
                db.execute(
                    "INSERT INTO runtime_observation_runs VALUES(?,?,?,'completed',?,?,?,?,NULL,CURRENT_TIMESTAMP)",
                    (run_id, sv_id, pkg_id, str(run_id)[-1] * 64, "b" * 64, image, guard),
                )
                db.execute(
                    "INSERT INTO runtime_discovery_schedule_state VALUES('p',?,'completed',?)",
                    (pkg_id, run_id),
                )
            db.commit()
            previous = auto_scheduler.previous_compatible_run(
                db, "p", 103, 3, "io.example/server", "example-mcp"
            )
            self.assertEqual(previous, 101)
            db.close()


if __name__ == "__main__":
    unittest.main()
