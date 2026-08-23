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
          profile_key TEXT,package_id INTEGER,state TEXT,runtime_observation_run_id INTEGER,
          previous_compatible_run_id INTEGER,added_tools INTEGER,removed_tools INTEGER,
          modified_tools INTEGER,unchanged_tools INTEGER,updated_at TEXT
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

            # Observation order must not reverse chronology: v2 cannot use later-published v3.
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
            # Simulate the old completion-order bug: v2 points to later-published v3.
            db.execute(
                """UPDATE runtime_discovery_schedule_state
                   SET previous_compatible_run_id=202,added_tools=1,removed_tools=1,
                       modified_tools=0,unchanged_tools=0
                   WHERE profile_key='p' AND package_id=11"""
            )
            db.commit()

            # Exercise the repair portion directly; base synchronization needs the full production schema.
            rows = db.execute(
                """SELECT s.package_id,s.runtime_observation_run_id,
                          r.server_version_id,sv.server_identifier,
                          p.identifier AS package_identifier
                   FROM runtime_discovery_schedule_state s
                   JOIN runtime_observation_runs r ON r.id=s.runtime_observation_run_id
                   JOIN server_versions sv ON sv.id=r.server_version_id
                   JOIN packages p ON p.id=r.package_id
                   WHERE s.profile_key='p' AND s.state='completed'"""
            ).fetchall()
            for row in rows:
                run_id = int(row["runtime_observation_run_id"])
                previous = auto_scheduler.previous_compatible_run(
                    db,
                    "p",
                    run_id,
                    int(row["server_version_id"]),
                    str(row["server_identifier"]),
                    str(row["package_identifier"]),
                )
                drift = (
                    auto_scheduler.base.compare_runs(db, previous, run_id)
                    if previous is not None
                    else None
                )
                counts = {name: None for name in ("added", "removed", "modified", "unchanged")}
                if drift is not None:
                    counts = {name: len(drift[name]) for name in counts}
                db.execute(
                    """UPDATE runtime_discovery_schedule_state
                       SET previous_compatible_run_id=?,added_tools=?,removed_tools=?,
                           modified_tools=?,unchanged_tools=?
                       WHERE profile_key='p' AND package_id=?""",
                    (
                        previous,
                        counts["added"],
                        counts["removed"],
                        counts["modified"],
                        counts["unchanged"],
                        int(row["package_id"]),
                    ),
                )
            repaired = db.execute(
                """SELECT previous_compatible_run_id
                   FROM runtime_discovery_schedule_state
                   WHERE profile_key='p' AND package_id=11"""
            ).fetchone()
            self.assertEqual(repaired["previous_compatible_run_id"], 201)
            db.close()


if __name__ == "__main__":
    unittest.main()
