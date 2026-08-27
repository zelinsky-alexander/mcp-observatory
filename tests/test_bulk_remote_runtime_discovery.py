#!/usr/bin/env python3
from __future__ import annotations

import importlib.util
from pathlib import Path
import sqlite3
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent


def load(name: str, path: str):
    spec = importlib.util.spec_from_file_location(name, ROOT / "tools" / path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


scheduler = load("bulk_remote_runtime_discovery", "bulk_remote_runtime_discovery.py")
remote = load("remote_runtime_discovery_for_scheduler_test", "remote_runtime_discovery.py")


class BulkRemoteRuntimeDiscoveryTests(unittest.TestCase):
    def test_stale_running_schedule_and_observation_are_recovered(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            database = Path(temporary) / "history.sqlite"
            db = sqlite3.connect(database)
            db.row_factory = sqlite3.Row
            db.executescript(
                """
                PRAGMA foreign_keys=ON;
                CREATE TABLE server_versions(
                  id INTEGER PRIMARY KEY,server_identifier TEXT,server_version TEXT
                );
                CREATE TABLE remotes(
                  id INTEGER PRIMARY KEY,server_version_id INTEGER REFERENCES server_versions(id),
                  position INTEGER,url TEXT,scheme TEXT,host TEXT,port INTEGER,transport TEXT
                );
                INSERT INTO server_versions VALUES(1,'fixture/server','1.0.0');
                INSERT INTO remotes VALUES(
                  10,1,0,'https://example.test/mcp','https','example.test',443,'streamable-http'
                );
                """
            )
            db.executescript(remote.SCHEMA)
            db.executescript(scheduler.SCHEMA)
            db.execute(
                """INSERT INTO runtime_remote_schedule_profiles(
                     profile_key,scheduler_version,probe_profile_sha256,runner_sha256)
                   VALUES('profile','1','a','b')"""
            )
            db.execute(
                "INSERT INTO runtime_remote_schedule_current VALUES(1,'profile',CURRENT_TIMESTAMP)"
            )
            cursor = db.execute(
                """INSERT INTO runtime_remote_observation_runs(
                     server_version_id,remote_id,status,declared_url,transport,probe_profile_sha256,
                     started_at)
                   VALUES(1,10,'running','https://example.test/mcp','streamable-http','a',
                          datetime('now','-3 hours'))"""
            )
            run_id = int(cursor.lastrowid)
            db.execute(
                """INSERT INTO runtime_remote_schedule_state(
                     profile_key,remote_id,state,attempt_count,runtime_remote_observation_run_id,
                     last_attempt_at)
                   VALUES('profile',10,'running',1,?,datetime('now','-3 hours'))""",
                (run_id,),
            )
            db.commit()

            with db:
                scheduler.recover_stale_running(db, "profile", 7200)

            state = db.execute(
                """SELECT state,reason_code FROM runtime_remote_schedule_state
                   WHERE profile_key='profile' AND remote_id=10"""
            ).fetchone()
            observation = db.execute(
                "SELECT status,error_stage FROM runtime_remote_observation_runs WHERE id=?",
                (run_id,),
            ).fetchone()
            self.assertEqual(tuple(state), ("inconclusive", "remote_interrupted"))
            self.assertEqual(tuple(observation), ("inconclusive", "interrupted"))
            db.close()


if __name__ == "__main__":
    unittest.main()
