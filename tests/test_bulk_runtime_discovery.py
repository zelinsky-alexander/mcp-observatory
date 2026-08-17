#!/usr/bin/env python3
"""Offline contract tests for the automatic runtime discovery scheduler."""

from __future__ import annotations

import argparse
import importlib.util
import json
from pathlib import Path
import sqlite3
import tempfile


ROOT = Path(__file__).resolve().parent.parent
SPEC = importlib.util.spec_from_file_location(
    "bulk_runtime_discovery", ROOT / "tools" / "bulk_runtime_discovery.py"
)
assert SPEC is not None and SPEC.loader is not None
scheduler = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(scheduler)


def make_base_database(path: Path) -> sqlite3.Connection:
    db = sqlite3.connect(path)
    db.row_factory = sqlite3.Row
    db.execute("PRAGMA foreign_keys=ON")
    db.executescript(
        """
        CREATE TABLE server_versions(
          id INTEGER PRIMARY KEY,
          server_identifier TEXT NOT NULL,
          server_version TEXT NOT NULL
        );
        CREATE TABLE packages(
          id INTEGER PRIMARY KEY,
          server_version_id INTEGER NOT NULL REFERENCES server_versions(id),
          registry_type TEXT,
          identifier TEXT,
          version TEXT,
          transport TEXT
        );
        CREATE TABLE runtime_observation_runs(
          id INTEGER PRIMARY KEY,
          server_version_id INTEGER NOT NULL REFERENCES server_versions(id),
          package_id INTEGER NOT NULL REFERENCES packages(id),
          status TEXT NOT NULL,
          artifact_sha256 TEXT,
          launch_profile_sha256 TEXT,
          sandbox_image TEXT NOT NULL,
          guard_version TEXT NOT NULL,
          inventory_sha256 TEXT,
          inventory_json TEXT,
          started_at TEXT DEFAULT CURRENT_TIMESTAMP,
          completed_at TEXT,
          error_stage TEXT,
          error_message TEXT
        );
        CREATE TABLE runtime_observation_tools(
          run_id INTEGER NOT NULL REFERENCES runtime_observation_runs(id),
          name TEXT NOT NULL,
          definition_json TEXT NOT NULL,
          definition_sha256 TEXT NOT NULL,
          PRIMARY KEY(run_id,name)
        );
        """
    )
    return db


def test_classification() -> None:
    def row(registry: str, transport: str, identifier: str, version: str) -> dict[str, str]:
        return {
            "registry_type": registry,
            "transport": transport,
            "identifier": identifier,
            "version": version,
        }

    assert scheduler.classify(row("npm", "stdio", "pkg", "1.0.0"))[0] == "eligible"
    assert scheduler.classify(row("pypi", "stdio", "pkg", "1.0.0"))[:2] == (
        "unsupported",
        "unsupported_ecosystem",
    )
    assert scheduler.classify(row("npm", "sse", "pkg", "1.0.0"))[:2] == (
        "unsupported",
        "unsupported_transport",
    )
    assert scheduler.classify(row("npm", "stdio", "", "1.0.0"))[:2] == (
        "unresolvable",
        "missing_package_identifier",
    )
    assert scheduler.classify(row("npm", "stdio", "pkg", ""))[:2] == (
        "unresolvable",
        "missing_exact_version",
    )


def test_profile_identity() -> None:
    with tempfile.TemporaryDirectory(prefix="mcpo-runtime-profile-") as temporary:
        root = Path(temporary)
        guard = root / "guard"
        runner = root / "runtime_discovery.py"
        probe = root / "probe.json"
        guard.write_bytes(b"guard-v1")
        runner.write_bytes(b"runner-v1")
        probe.write_text(
            json.dumps(
                {
                    "profile_version": 1,
                    "profile_id": "test-discovery-v1",
                    "transport": "stdio",
                    "guard_command": "inspect",
                    "tool_invocation": False,
                    "inventory_version": 1,
                },
                sort_keys=True,
            ),
            encoding="utf-8",
        )
        args = argparse.Namespace(
            guard_binary=str(guard),
            runtime_runner=str(runner),
            probe_profile=str(probe),
            runtime_image="node:test",
        )
        profile, key = scheduler.load_profile(args)
        assert len(key) == 64
        assert len(profile["guard_sha256"]) == 64
        assert len(profile["probe_profile_sha256"]) == 64

        guard.write_bytes(b"guard-v2")
        changed, changed_key = scheduler.load_profile(args)
        assert changed["guard_sha256"] != profile["guard_sha256"]
        assert changed_key != key


def test_schedule_and_drift() -> None:
    with tempfile.TemporaryDirectory(prefix="mcpo-runtime-scheduler-") as temporary:
        database = Path(temporary) / "catalog.sqlite"
        db = make_base_database(database)
        db.executescript(
            """
            INSERT INTO server_versions VALUES(1,'io.example/server','1.0.0');
            INSERT INTO server_versions VALUES(2,'io.example/server','2.0.0');
            INSERT INTO server_versions VALUES(3,'io.example/other','1.0.0');
            INSERT INTO packages VALUES(10,1,'npm','example-mcp','1.0.0','stdio');
            INSERT INTO packages VALUES(11,2,'npm','example-mcp','2.0.0','stdio');
            INSERT INTO packages VALUES(12,3,'pypi','other','1.0.0','stdio');
            INSERT INTO packages VALUES(13,3,'npm','remote-only','1.0.0','sse');
            """
        )
        profile = {
            "scheduler_version": "1.0.0",
            "guard_sha256": "a" * 64,
            "runtime_image": "node:test",
            "probe_profile_sha256": "b" * 64,
            "runner_sha256": "c" * 64,
        }
        key = "d" * 64
        with db:
            scheduler.register_profile(db, profile, key)
            scheduler.synchronize(db, key, profile, stale_seconds=3600)

        states = {
            int(row["package_id"]): (row["state"], row["reason_code"])
            for row in db.execute(
                "SELECT package_id,state,reason_code FROM runtime_discovery_schedule_state"
            )
        }
        assert states[10][0] == "eligible"
        assert states[11][0] == "eligible"
        assert states[12] == ("unsupported", "unsupported_ecosystem")
        assert states[13] == ("unsupported", "unsupported_transport")

        artifact1, artifact2 = "1" * 64, "2" * 64
        launch1, launch2 = "3" * 64, "4" * 64
        guard = "sha256:" + profile["guard_sha256"]
        with db:
            db.execute(
                """INSERT INTO runtime_observation_runs(
                     id,server_version_id,package_id,status,artifact_sha256,
                     launch_profile_sha256,sandbox_image,guard_version,completed_at)
                   VALUES(101,1,10,'completed',?,?,?,?,CURRENT_TIMESTAMP)""",
                (artifact1, launch1, profile["runtime_image"], guard),
            )
            db.execute(
                """INSERT INTO runtime_observation_runs(
                     id,server_version_id,package_id,status,artifact_sha256,
                     launch_profile_sha256,sandbox_image,guard_version,completed_at)
                   VALUES(102,2,11,'completed',?,?,?,?,CURRENT_TIMESTAMP)""",
                (artifact2, launch2, profile["runtime_image"], guard),
            )
            old_tools = {
                "read": '{"name":"read","inputSchema":{"type":"object"}}',
                "old": '{"name":"old"}',
            }
            new_tools = {
                "read": '{"name":"read","inputSchema":{"type":"object","required":["path"]}}',
                "new": '{"name":"new"}',
            }
            for run_id, tools in ((101, old_tools), (102, new_tools)):
                for name, definition in tools.items():
                    db.execute(
                        "INSERT INTO runtime_observation_tools VALUES(?,?,?,?)",
                        (run_id, name, definition, "f" * 64),
                    )
            db.execute(
                """UPDATE runtime_discovery_schedule_state
                   SET state='completed',runtime_observation_run_id=101,
                       artifact_sha256=?,launch_profile_sha256=?
                   WHERE profile_key=? AND package_id=10""",
                (artifact1, launch1, key),
            )
            db.execute(
                """UPDATE runtime_discovery_schedule_state
                   SET state='completed',runtime_observation_run_id=102,
                       artifact_sha256=?,launch_profile_sha256=?
                   WHERE profile_key=? AND package_id=11""",
                (artifact2, launch2, key),
            )

        previous = scheduler.previous_compatible_run(
            db,
            key,
            current_run_id=102,
            current_server_version_id=2,
            server_identifier="io.example/server",
            package_identifier="example-mcp",
        )
        assert previous == 101
        drift = scheduler.compare_runs(db, 101, 102)
        assert drift["added"] == ["new"]
        assert drift["removed"] == ["old"]
        assert drift["modified"] == ["read"]
        assert drift["unchanged"] == []

        with db:
            scheduler.set_result(
                db,
                key,
                11,
                "completed",
                None,
                None,
                run_id=102,
                artifact=artifact2,
                launch_profile=launch2,
                previous_run_id=101,
                drift=drift,
            )
        result = scheduler.summary(db, key)
        assert result["completed_observations"] == 2
        assert result["comparable_observations"] == 1
        assert result["drifted_observations"] == 1
        db.close()


def main() -> None:
    test_classification()
    test_profile_identity()
    test_schedule_and_drift()
    print("bulk runtime discovery tests passed")


if __name__ == "__main__":
    main()
