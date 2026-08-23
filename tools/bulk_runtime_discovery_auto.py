#!/usr/bin/env python3
"""Automatic-runtime compatibility layer for bulk_runtime_discovery.py.

The base scheduler still owns bounded claiming, retries, persistence, and profile
identity. This layer adds automatic Node-image compatibility, publication-time drift
chronology, prerequisite-aware launch classification, and explicit blocked /
inconclusive outcomes so true protocol failures are not conflated with servers that
cannot be meaningfully started under the declared zero-secret launch context.
"""

from __future__ import annotations

import importlib.util
from pathlib import Path
import sqlite3
import sys
from typing import Any

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

base.SCHEMA = base.SCHEMA.replace(
    "'eligible','running','completed','failed','unsupported','unresolvable'",
    "'eligible','running','completed','failed','unsupported','unresolvable','blocked','inconclusive'",
)

STATE_COLUMNS = (
    "profile_key,package_id,state,reason_code,reason_message,attempt_count,"
    "runtime_observation_run_id,artifact_sha256,launch_profile_sha256,"
    "previous_compatible_run_id,added_tools,removed_tools,modified_tools,"
    "unchanged_tools,discovered_at,last_attempt_at,updated_at"
)

STATE_TABLE_SQL = """
CREATE TABLE runtime_discovery_schedule_state(
  profile_key TEXT NOT NULL REFERENCES runtime_discovery_schedule_profiles(profile_key)
    ON DELETE CASCADE,
  package_id INTEGER NOT NULL REFERENCES packages(id) ON DELETE CASCADE,
  state TEXT NOT NULL CHECK(state IN(
    'eligible','running','completed','failed','unsupported','unresolvable','blocked','inconclusive'
  )),
  reason_code TEXT,
  reason_message TEXT,
  attempt_count INTEGER NOT NULL DEFAULT 0 CHECK(attempt_count >= 0),
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
)
"""

INDEX_SQL = """
CREATE INDEX runtime_discovery_schedule_state_lookup
ON runtime_discovery_schedule_state(
  profile_key,state,discovered_at,attempt_count,last_attempt_at,package_id
)
"""

# These all require the server to have produced or meaningfully progressed to an
# MCP response. They are the only ordinary discovery outcomes stored as `failed`.
PROTOCOL_FAILURE_CODES = {
    "fail malformed_json": "protocol_malformed_json",
    "fail wrong_response_id": "protocol_wrong_response_id",
    "fail unsupported_jsonrpc_version": "protocol_unsupported_jsonrpc",
    "fail oversized message": "protocol_oversized_message",
    "fail unsolicited message": "protocol_unsolicited_message",
    "fail tools/list timeout": "protocol_tools_list_timeout",
    "fail tools/list write": "protocol_tools_list_write_failed",
    "fail tools/list receive": "protocol_tools_list_receive_failed",
}

# Failures before a meaningful MCP response are not protocol verdicts.
INCONCLUSIVE_GUARD_CODES = {
    "fail child exited early": "startup_child_exited_early",
    "fail initialize timeout": "startup_initialize_timeout",
    "fail initialize write": "startup_initialize_write_failed",
    "fail initialize receive": "startup_initialize_receive_failed",
    "fail downstream process start": "startup_process_start_failed",
}


def ensure_outcome_states(db: sqlite3.Connection) -> None:
    """One-time in-place migration of the scheduler state CHECK constraint."""
    row = db.execute(
        "SELECT sql FROM sqlite_schema WHERE type='table' AND name='runtime_discovery_schedule_state'"
    ).fetchone()
    if row is None:
        return
    definition = str(row["sql"] or "")
    if "'blocked'" in definition and "'inconclusive'" in definition:
        return

    legacy = "runtime_discovery_schedule_state_pre_outcomes"
    if base.table_exists(db, legacy):
        raise RuntimeError("incomplete runtime outcome-state migration exists")
    db.execute("DROP INDEX IF EXISTS runtime_discovery_schedule_state_lookup")
    db.execute(f"ALTER TABLE runtime_discovery_schedule_state RENAME TO {legacy}")
    db.execute(STATE_TABLE_SQL)
    db.execute(
        f"INSERT INTO runtime_discovery_schedule_state({STATE_COLUMNS}) "
        f"SELECT {STATE_COLUMNS} FROM {legacy}"
    )
    db.execute(f"DROP TABLE {legacy}")
    db.execute(INDEX_SQL)


def package_prerequisite(
    db: sqlite3.Connection, package_id: int
) -> tuple[str, str, str] | None:
    """Return a terminal blocked outcome for declared prerequisites we cannot satisfy."""
    if base.table_exists(db, "package_environment"):
        required = [
            str(row["name"])
            for row in db.execute(
                """SELECT name FROM package_environment
                   WHERE package_id=? AND required<>0 ORDER BY position""",
                (package_id,),
            )
            if str(row["name"] or "").strip()
        ]
        if required:
            names = ", ".join(required[:16])
            return (
                "blocked",
                "blocked_required_environment",
                "declared required environment unavailable in zero-secret probe: " + names,
            )

    if base.table_exists(db, "package_arguments"):
        missing = db.execute(
            """SELECT position FROM package_arguments
               WHERE package_id=? AND argument_value IS NULL
               ORDER BY position LIMIT 1""",
            (package_id,),
        ).fetchone()
        if missing is not None:
            return (
                "blocked",
                "blocked_required_argument",
                f"declared launch argument at position {int(missing['position'])} has no value",
            )
    return None


def startup_block_reason(stderr: str) -> str | None:
    """Conservatively recognize obvious prerequisite diagnostics from bounded stderr."""
    lowered = stderr.lower()
    if "server stderr:" not in lowered:
        return None
    server = lowered.split("server stderr:", 1)[1]

    environment_terms = (
        "environment variable", "env var", "api key", "apikey", "credential",
        "access token", "auth token", "database url", "database_url",
    )
    requirement_terms = (
        "required", "missing", "must be set", "not set", "please set",
        "not configured", "must provide", "please provide",
    )
    if any(term in server for term in environment_terms) and any(
        term in server for term in requirement_terms
    ):
        return "blocked_startup_environment"

    if any(term in server for term in ("config file", "configuration file", "configuration")) and any(
        term in server for term in requirement_terms
    ):
        return "blocked_startup_configuration"

    if "usage:" in server or (
        any(term in server for term in ("argument", "option", "flag"))
        and any(term in server for term in requirement_terms)
    ):
        return "blocked_startup_arguments"
    return None


def classify_child_failure(stderr: str, returncode: int) -> tuple[str, str, str]:
    """Classify server protocol verdicts separately from launch/harness limitations."""
    lowered = stderr.lower()

    if "required environment unavailable:" in lowered:
        return "blocked", "blocked_required_environment", stderr
    if "required declared argument has no value" in lowered:
        return "blocked", "blocked_required_argument", stderr

    if "mcp-native-guard inspect failed" in lowered:
        blocked = startup_block_reason(stderr)
        for marker, code in INCONCLUSIVE_GUARD_CODES.items():
            if marker in lowered:
                if blocked is not None:
                    return "blocked", blocked, stderr
                return "inconclusive", code, stderr
        for marker, code in PROTOCOL_FAILURE_CODES.items():
            if marker in lowered:
                return "failed", code, stderr
        # Unknown Guard stage is not enough evidence for a protocol verdict.
        return "inconclusive", "protocol_stage_unknown", stderr

    if "http error 404" in lowered or "http error 410" in lowered:
        return "unresolvable", "artifact_unresolvable", stderr
    if "no approved node runtime" in lowered:
        return "unresolvable", "unsupported_runtime", stderr

    state, code, message = base_classify_child_failure(stderr, returncode)
    if state != "failed":
        return state, code, message
    if code == "install_failed" or "cache population failed" in lowered:
        return "inconclusive", "runtime_install_failed", message
    # Child timeouts/output bounds/other harness errors are operationally
    # inconclusive; they are not evidence that the MCP protocol failed.
    return "inconclusive", "runtime_harness_failed", message


def refine_persisted_outcomes(db: sqlite3.Connection, key: str) -> int:
    """Apply prerequisite/startup semantics without creating another runtime attempt."""
    rows = db.execute(
        """SELECT package_id,state,reason_code,reason_message
           FROM runtime_discovery_schedule_state
           WHERE profile_key=? AND state<>'completed'""",
        (key,),
    ).fetchall()
    changed = 0
    for row in rows:
        package_id = int(row["package_id"])
        prerequisite = package_prerequisite(db, package_id)
        if prerequisite is not None:
            state, code, message = prerequisite
        elif row["state"] == "failed" and row["reason_message"]:
            state, code, message = classify_child_failure(str(row["reason_message"]), 1)
        else:
            continue
        if state == row["state"] and code == row["reason_code"]:
            continue
        cursor = db.execute(
            """UPDATE runtime_discovery_schedule_state
               SET state=?,reason_code=?,reason_message=?,updated_at=CURRENT_TIMESTAMP
               WHERE profile_key=? AND package_id=?""",
            (state, code, base.bounded_message(message), key, package_id),
        )
        changed += int(cursor.rowcount or 0)
    return changed


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


base_classify_child_failure = base.classify_child_failure
_original_register_profile = base.register_profile
_original_synchronize = base.synchronize
_original_set_result = base.set_result


def register_profile(db: sqlite3.Connection, profile: dict[str, str], key: str) -> None:
    ensure_outcome_states(db)
    _original_register_profile(db, profile, key)


def set_result(
    db: sqlite3.Connection,
    key: str,
    package_id: int,
    state: str,
    code: str | None,
    message: str | None,
    **kwargs: Any,
) -> None:
    """Keep internal result-verification errors out of the protocol-failure bucket."""
    if state == "failed" and code == "result_verification_failed":
        state = "inconclusive"
        code = "runtime_result_verification_failed"
    _original_set_result(db, key, package_id, state, code, message, **kwargs)


def synchronize(
    db: sqlite3.Connection,
    key: str,
    profile: dict[str, str],
    stale_seconds: int,
) -> None:
    """Synchronize state, classify prerequisites, and repair drift chronology."""
    _original_synchronize(db, key, profile, stale_seconds)
    refine_persisted_outcomes(db, key)

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
        counts = {name: None for name in ("added", "removed", "modified", "unchanged")}
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


def summary(db: sqlite3.Connection, key: str) -> dict[str, Any]:
    row = db.execute(
        """SELECT COUNT(*) total_package_records,
                  SUM(state IN('eligible','running','completed','failed','blocked','inconclusive')) eligible_package_records,
                  SUM(state='completed') completed_observations,
                  SUM(state='failed') failed_attempts,
                  SUM(state='blocked') blocked_observations,
                  SUM(state='inconclusive') inconclusive_observations,
                  SUM(state IN('unsupported','unresolvable')) unsupported_or_unresolvable,
                  SUM(state='eligible' AND attempt_count=0) never_attempted,
                  COUNT(DISTINCT CASE WHEN state='completed' THEN artifact_sha256 END)
                    unique_artifacts_observed,
                  SUM(state='running') running,
                  SUM(state='completed' AND previous_compatible_run_id IS NOT NULL)
                    comparable_observations,
                  SUM(state='completed' AND previous_compatible_run_id IS NOT NULL
                      AND (COALESCE(added_tools,0)+COALESCE(removed_tools,0)+
                           COALESCE(modified_tools,0))>0) drifted_observations
           FROM runtime_discovery_schedule_state WHERE profile_key=?""",
        (key,),
    ).fetchone()
    result = {name: int(row[name] or 0) for name in row.keys()}
    result["profile_key"] = key
    return result


base.register_profile = register_profile
base._completed_state_is_valid = completed_state_is_valid
base.verify_run = verify_run
base.previous_compatible_run = previous_compatible_run
base.classify_child_failure = classify_child_failure
base.set_result = set_result
base.synchronize = synchronize
base.summary = summary


if __name__ == "__main__":
    raise SystemExit(base.main())
