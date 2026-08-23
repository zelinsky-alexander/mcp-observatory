#!/usr/bin/env python3
"""Automatic-runtime compatibility layer for bulk_runtime_discovery.py.

The base scheduler still owns bounded claiming, retries, persistence, drift counts,
and profile identity. This layer teaches it that the versioned auto-node policy may
resolve to one of a bounded set of concrete runtime images, that longitudinal
comparisons are compatible only when both observations used the same resolved image,
and that drift chronology follows registry publication time rather than scheduler
completion order.
"""

from __future__ import annotations

import importlib.util
from pathlib import Path
import sqlite3
import sys

HERE = Path(__file__).resolve().parent

BASE_SPEC = importlib.util.spec_from_file_location(
    "bulk_runtime_discovery_base", HERE / "bulk_runtime_discovery.py"
)
assert BASE_SPEC is not None and BASE_SPEC.loader is not None
base = importlib.util.module_from_spec(BASE_SPEC)
BASE_SPEC.loader.exec_module(base)

AUTO_SPEC = importlib.util.spec_from_file_location(
    "runtime_discovery_auto", HERE / "runtime_discovery_auto.py"
)
assert AUTO_SPEC is not None and AUTO_SPEC.loader is not None
auto = importlib.util.module_from_spec(AUTO_SPEC)
AUTO_SPEC.loader.exec_module(auto)


def allowed_images(profile: dict[str, str]) -> set[str]:
    policy = profile["runtime_image"]
    if policy != auto.AUTO_RUNTIME_POLICY:
        return {policy}
    return {image for _, image in auto.AUTO_RUNTIME_CANDIDATES}


def completed_state_is_valid(
    db: sqlite3.Connection,
    run_id: int | None,
    package_id: int,
    profile: dict[str, str],
) -> bool:
    if run_id is None or not base.table_exists(db, "runtime_observation_runs"):
        return False
    row = db.execute(
        """SELECT status,package_id,guard_version,sandbox_image,artifact_sha256,
                  launch_profile_sha256
           FROM runtime_observation_runs WHERE id=?""",
        (run_id,),
    ).fetchone()
    if row is None:
        return False
    return (
        row["status"] == "completed"
        and int(row["package_id"]) == package_id
        and row["guard_version"] == "sha256:" + profile["guard_sha256"]
        and row["sandbox_image"] in allowed_images(profile)
        and isinstance(row["artifact_sha256"], str)
        and isinstance(row["launch_profile_sha256"], str)
    )


def verify_run(
    db: sqlite3.Connection,
    run_id: int,
    package_id: int,
    artifact: str,
    launch_profile: str,
    profile: dict[str, str],
) -> None:
    row = db.execute(
        """SELECT status,package_id,artifact_sha256,launch_profile_sha256,
                  sandbox_image,guard_version
           FROM runtime_observation_runs WHERE id=?""",
        (run_id,),
    ).fetchone()
    if row is None:
        raise ValueError("runtime child result has no authoritative observation row")
    if (
        row["status"] != "completed"
        or int(row["package_id"]) != package_id
        or row["artifact_sha256"] != artifact
        or row["launch_profile_sha256"] != launch_profile
        or row["sandbox_image"] not in allowed_images(profile)
        or row["guard_version"] != "sha256:" + profile["guard_sha256"]
    ):
        raise ValueError("runtime child result does not match the authoritative observation row")


def previous_compatible_run(
    db: sqlite3.Connection,
    key: str,
    current_run_id: int,
    current_server_version_id: int,
    server_identifier: str,
    package_identifier: str,
) -> int | None:
    """Return the latest published earlier compatible version, never a later one."""
    if not base.table_exists(db, "runtime_observation_runs"):
        return None
    current = db.execute(
        """SELECT r.sandbox_image,sv.published_at
           FROM runtime_observation_runs r
           JOIN server_versions sv ON sv.id=r.server_version_id
           WHERE r.id=? AND r.server_version_id=?""",
        (current_run_id, current_server_version_id),
    ).fetchone()
    if current is None or not current["published_at"]:
        return None
    row = db.execute(
        """SELECT r.id
           FROM runtime_discovery_schedule_state s
           JOIN runtime_observation_runs r ON r.id=s.runtime_observation_run_id
           JOIN server_versions sv ON sv.id=r.server_version_id
           JOIN packages p ON p.id=r.package_id
           WHERE s.profile_key=? AND s.state='completed'
             AND r.status='completed' AND r.id<>?
             AND r.server_version_id<>?
             AND sv.server_identifier=? AND p.identifier=?
             AND r.sandbox_image=?
             AND sv.published_at IS NOT NULL
             AND sv.published_at < ?
           ORDER BY sv.published_at COLLATE BINARY DESC,
                    COALESCE(r.completed_at,r.started_at,'') COLLATE BINARY DESC,
                    r.id DESC
           LIMIT 1""",
        (
            key,
            current_run_id,
            current_server_version_id,
            server_identifier,
            package_identifier,
            current["sandbox_image"],
            current["published_at"],
        ),
    ).fetchone()
    return None if row is None else int(row["id"])


_original_synchronize = base.synchronize


def synchronize(
    db: sqlite3.Connection,
    key: str,
    profile: dict[str, str],
    stale_seconds: int,
) -> None:
    """Synchronize scheduler state and repair drift links by publication chronology."""
    _original_synchronize(db, key, profile, stale_seconds)
    columns = {
        str(row["name"])
        for row in db.execute("PRAGMA table_info(runtime_discovery_schedule_state)")
    }
    required = {
        "profile_key",
        "package_id",
        "state",
        "runtime_observation_run_id",
        "previous_compatible_run_id",
        "added_tools",
        "removed_tools",
        "modified_tools",
        "unchanged_tools",
    }
    if not required.issubset(columns):
        return
    rows = db.execute(
        """SELECT s.package_id,s.runtime_observation_run_id,
                  r.server_version_id,sv.server_identifier,
                  p.identifier AS package_identifier
           FROM runtime_discovery_schedule_state s
           JOIN runtime_observation_runs r ON r.id=s.runtime_observation_run_id
           JOIN server_versions sv ON sv.id=r.server_version_id
           JOIN packages p ON p.id=r.package_id
           WHERE s.profile_key=? AND s.state='completed'
             AND s.runtime_observation_run_id IS NOT NULL""",
        (key,),
    ).fetchall()
    for row in rows:
        run_id = int(row["runtime_observation_run_id"])
        previous = previous_compatible_run(
            db,
            key,
            run_id,
            int(row["server_version_id"]),
            str(row["server_identifier"]),
            str(row["package_identifier"]),
        )
        drift = base.compare_runs(db, previous, run_id) if previous is not None else None
        counts = {
            "added": None,
            "removed": None,
            "modified": None,
            "unchanged": None,
        }
        if drift is not None:
            counts = {name: len(drift[name]) for name in counts}
        db.execute(
            """UPDATE runtime_discovery_schedule_state
               SET previous_compatible_run_id=?,added_tools=?,removed_tools=?,
                   modified_tools=?,unchanged_tools=?,updated_at=CURRENT_TIMESTAMP
               WHERE profile_key=? AND package_id=?""",
            (
                previous,
                counts["added"],
                counts["removed"],
                counts["modified"],
                counts["unchanged"],
                key,
                int(row["package_id"]),
            ),
        )


base._completed_state_is_valid = completed_state_is_valid
base.verify_run = verify_run
base.previous_compatible_run = previous_compatible_run
base.synchronize = synchronize


if __name__ == "__main__":
    raise SystemExit(base.main())
