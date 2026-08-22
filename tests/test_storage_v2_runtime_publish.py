#!/usr/bin/env python3
"""Offline tests for Storage v2 runtime read-model publication."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import sqlite3
import tempfile


ROOT = Path(__file__).resolve().parent.parent


def load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


publisher = load(
    "storage_v2_runtime_publish", ROOT / "tools" / "storage_v2_runtime_publish.py"
)
runtime = load("runtime_discovery", ROOT / "tools" / "runtime_discovery.py")
scheduler = load("bulk_runtime_discovery", ROOT / "tools" / "bulk_runtime_discovery.py")


def base_schema(db: sqlite3.Connection) -> None:
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
        INSERT INTO server_versions VALUES(1,'io.example/server','1.0.0');
        INSERT INTO packages VALUES(10,1,'npm','example-mcp','1.0.0','stdio');
        """
    )


def test_first_runtime_publication_installs_hot_schema() -> None:
    with tempfile.TemporaryDirectory(prefix="mcpo-runtime-publish-") as temporary:
        root = Path(temporary)
        history = root / "history.sqlite"
        hot = root / "hot.sqlite"
        h = sqlite3.connect(history)
        p = sqlite3.connect(hot)
        base_schema(h)
        base_schema(p)
        h.executescript(runtime.SCHEMA)
        h.executescript(scheduler.SCHEMA)
        h.execute(
            """INSERT INTO runtime_discovery_schedule_profiles(
                 profile_key,scheduler_version,guard_sha256,runtime_image,
                 probe_profile_sha256,runner_sha256)
               VALUES(?,?,?,?,?,?)""",
            ("a" * 64, "1.0.0", "b" * 64, "node:test", "c" * 64, "d" * 64),
        )
        h.execute(
            "INSERT INTO runtime_discovery_schedule_current(singleton,profile_key) VALUES(1,?)",
            ("a" * 64,),
        )
        h.execute(
            """INSERT INTO runtime_observation_runs(
                 id,server_version_id,package_id,status,artifact_sha256,
                 launch_profile_sha256,sandbox_image,guard_version,
                 inventory_sha256,inventory_json,completed_at)
               VALUES(101,1,10,'completed',?,?,?,?,?,?,CURRENT_TIMESTAMP)""",
            (
                "1" * 64,
                "2" * 64,
                "node:test",
                "sha256:" + "b" * 64,
                "3" * 64,
                '{"inventory_version":1,"tools":[]}',
            ),
        )
        h.execute(
            "INSERT INTO runtime_observation_tools VALUES(101,'read','{\"name\":\"read\"}',?)",
            ("4" * 64,),
        )
        h.execute(
            """INSERT INTO runtime_discovery_schedule_state(
                 profile_key,package_id,state,runtime_observation_run_id,
                 artifact_sha256,launch_profile_sha256)
               VALUES(?,10,'completed',101,?,?)""",
            ("a" * 64, "1" * 64, "2" * 64),
        )
        h.commit()
        h.close()
        p.close()

        result = publisher.mirror(history, hot)
        assert result["runtime_observation_runs"] == 1
        assert result["runtime_observation_tools"] == 1
        assert result["runtime_discovery_schedule_state"] == 1

        p = sqlite3.connect(hot)
        assert p.execute("SELECT COUNT(*) FROM runtime_observation_runs").fetchone()[0] == 1
        assert p.execute("SELECT COUNT(*) FROM runtime_observation_tools").fetchone()[0] == 1
        assert p.execute("SELECT state FROM runtime_discovery_schedule_state").fetchone()[0] == "completed"
        p.close()


def main() -> None:
    test_first_runtime_publication_installs_hot_schema()
    print("Storage v2 runtime publish tests passed")


if __name__ == "__main__":
    main()
