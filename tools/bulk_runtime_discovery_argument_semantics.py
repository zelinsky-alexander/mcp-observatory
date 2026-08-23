#!/usr/bin/env python3
"""Conservative scheduler semantics for Registry package arguments and startup outcomes.

The current catalog stores argument position/value but not ``isRequired``. A NULL
argument value is therefore not sufficient evidence for a blocked launch. Required
environment declarations remain terminal blockers because their required bit is
preserved. Missing arguments are attempted without invention.

This layer also turns bounded server stderr into compact, explicit startup outcome
subtypes so durable scheduler records preserve the useful Guard/server diagnostic
instead of a Python traceback.
"""

from __future__ import annotations

import importlib.util
from pathlib import Path
import sqlite3

HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location(
    "bulk_runtime_discovery_auto_base", HERE / "bulk_runtime_discovery_auto.py"
)
assert SPEC is not None and SPEC.loader is not None
scheduler = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(scheduler)


def package_prerequisite(
    db: sqlite3.Connection, package_id: int
) -> tuple[str, str, str] | None:
    """Pre-block only prerequisites whose required semantics are actually retained."""
    if scheduler.base.table_exists(db, "package_environment"):
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

    # package_arguments currently lacks the Registry isRequired bit. NULL value
    # therefore means unknown/user-supplied, not necessarily required.
    return None


def compact_runtime_message(stderr: str) -> str:
    """Discard wrapper traceback noise while retaining bounded Guard/server evidence."""
    text = stderr.replace("\r\n", "\n").replace("\r", "\n").strip()
    marker = "RuntimeError: "
    position = text.rfind(marker)
    if position >= 0:
        text = text[position + len(marker):].strip()
    # Child stderr can occasionally contain repeated blank lines; collapse only
    # whitespace-only line runs, preserving the diagnostic text itself.
    lines = [line.rstrip() for line in text.splitlines()]
    compact: list[str] = []
    previous_blank = False
    for line in lines:
        blank = not line.strip()
        if blank and previous_blank:
            continue
        compact.append(line)
        previous_blank = blank
    return "\n".join(compact).strip()


def _server_stderr(message: str) -> str:
    lowered = message.lower()
    marker = "server stderr:"
    position = lowered.find(marker)
    if position < 0:
        return ""
    return message[position + len(marker):].strip()


def startup_subtype(message: str) -> tuple[str, str] | None:
    """Conservatively classify explicit startup prerequisites from server stderr."""
    server = _server_stderr(message)
    if not server:
        return None
    lowered = server.lower()

    # Missing package/module content in the exact installed artifact is not a
    # prerequisite we can supply, and is not an MCP protocol verdict.
    if (
        "err_module_not_found" in lowered
        or "cannot find module '/work/node_modules/" in lowered
        or 'cannot find module "/work/node_modules/' in lowered
    ):
        return "inconclusive", "inconclusive_startup_package_error"

    # Explicit network/DNS access is impossible in the intentionally network-none
    # runtime profile. This is a profile prerequisite, not a server protocol failure.
    if any(
        term in lowered
        for term in (
            "eai_again",
            "enotfound",
            "getaddrinfo",
            "network is unreachable",
            "econnrefused",
        )
    ):
        return "blocked", "blocked_startup_network"

    # Explicit inability to create/write a home/config path under the read-only
    # container is a writable-path prerequisite.
    if (
        "read-only file system" in lowered
        or "erofs" in lowered
        or ("enoent" in lowered and "mkdir '/." in lowered)
        or ("enoent" in lowered and 'mkdir "/.' in lowered)
    ):
        return "blocked", "blocked_startup_writable_path"

    if (
        "auto_mod_root_not_found" in lowered
        or "start the mcp from inside" in lowered
        or "server workspace" in lowered
        or "workspace" in lowered and any(
            term in lowered for term in ("required", "configure", "not found", "missing")
        )
    ):
        return "blocked", "blocked_startup_workspace"

    if (
        "could not find the nimbus cli" in lowered
        or ("could not find" in lowered and " cli" in lowered)
        or ("install it" in lowered and "_bin" in lowered)
    ):
        return "blocked", "blocked_startup_external_binary"

    if (
        "unknown argument" in lowered
        or "unknown option" in lowered
        or "unrecognized argument" in lowered
        or "unrecognized option" in lowered
        or "usage:" in lowered
    ):
        return "blocked", "blocked_startup_arguments"

    if (
        "no configuration file found" in lowered
        or "configuration file" in lowered and any(
            term in lowered for term in ("missing", "not found", "required", "please edit")
        )
    ):
        return "blocked", "blocked_startup_configuration"

    # Retain the older generic prerequisite recognizer for credentials/config/args.
    legacy = scheduler.startup_block_reason(message)
    if legacy is not None:
        return "blocked", legacy
    return None


_base_classify_child_failure = scheduler.classify_child_failure


def classify_child_failure(stderr: str, returncode: int) -> tuple[str, str, str]:
    """Classify startup subtypes and persist only compact diagnostic evidence."""
    compact = compact_runtime_message(stderr)
    lowered = compact.lower()

    if "mcp-native-guard inspect failed" in lowered and "fail child exited early" in lowered:
        subtype = startup_subtype(compact)
        if subtype is not None:
            state, code = subtype
            return state, code, compact
        return "inconclusive", "startup_child_exited_early", compact

    state, code, _ = _base_classify_child_failure(stderr, returncode)
    return state, code, compact


def refine_persisted_outcomes(db: sqlite3.Connection, key: str) -> int:
    """Reclassify current-profile startup rows and compact their stored diagnostics."""
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
        elif row["reason_message"] and row["state"] in (
            "failed", "inconclusive", "blocked"
        ):
            message = str(row["reason_message"])
            state, code, message = classify_child_failure(message, 1)
        else:
            continue

        bounded = scheduler.base.bounded_message(message)
        if (
            state == row["state"]
            and code == row["reason_code"]
            and bounded == row["reason_message"]
        ):
            continue
        cursor = db.execute(
            """UPDATE runtime_discovery_schedule_state
               SET state=?,reason_code=?,reason_message=?,updated_at=CURRENT_TIMESTAMP
               WHERE profile_key=? AND package_id=?""",
            (state, code, bounded, key, package_id),
        )
        changed += int(cursor.rowcount or 0)
    return changed


scheduler.package_prerequisite = package_prerequisite
scheduler.classify_child_failure = classify_child_failure
scheduler.base.classify_child_failure = classify_child_failure
scheduler.refine_persisted_outcomes = refine_persisted_outcomes

# bulk_runtime_discovery_auto.synchronize resolves refine_persisted_outcomes through
# its module globals, so replacing it above also upgrades already-persisted rows.


if __name__ == "__main__":
    raise SystemExit(scheduler.base.main())
