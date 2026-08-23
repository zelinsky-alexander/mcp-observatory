#!/usr/bin/env python3
"""Conservative scheduler semantics for Registry package arguments.

The current catalog stores argument position/value but not ``isRequired``. A NULL
argument value is therefore not sufficient evidence for a blocked launch. Required
environment declarations remain terminal blockers because their required bit is
preserved. Missing arguments are attempted without invention and may later become a
bounded stderr-derived ``blocked_startup_arguments`` outcome.
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


scheduler.package_prerequisite = package_prerequisite


if __name__ == "__main__":
    raise SystemExit(scheduler.base.main())
