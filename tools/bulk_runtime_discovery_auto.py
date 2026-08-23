#!/usr/bin/env python3
"""Automatic-runtime compatibility layer for bulk_runtime_discovery.py.

The base scheduler still owns bounded claiming, retries, persistence, drift counts,
and profile identity. This layer only teaches it that the versioned auto-node policy
may resolve to one of a bounded set of concrete runtime images, and that longitudinal
comparisons are compatible only when both observations used the same resolved image.
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
    if not base.table_exists(db, "runtime_observation_runs"):
        return None
    current = db.execute(
        "SELECT sandbox_image FROM runtime_observation_runs WHERE id=?",
        (current_run_id,),
    ).fetchone()
    if current is None:
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
           ORDER BY COALESCE(r.completed_at,r.started_at,'') COLLATE BINARY DESC,
                    r.id DESC
           LIMIT 1""",
        (
            key,
            current_run_id,
            current_server_version_id,
            server_identifier,
            package_identifier,
            current["sandbox_image"],
        ),
    ).fetchone()
    return None if row is None else int(row["id"])


base._completed_state_is_valid = completed_state_is_valid
base.verify_run = verify_run
base.previous_compatible_run = previous_compatible_run


if __name__ == "__main__":
    raise SystemExit(base.main())
