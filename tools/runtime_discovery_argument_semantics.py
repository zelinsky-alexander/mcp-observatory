#!/usr/bin/env python3
"""Conservative Registry argument semantics for automatic runtime discovery.

The current catalog projection preserves package-argument positions and concrete
values, but does not preserve the Registry argument ``isRequired`` flag. Therefore
a NULL ``argument_value`` is *unknown*, not evidence that launch is blocked. This
layer passes only concrete declared argv values and lets bounded server stderr
identify truly required arguments at startup.
"""

from __future__ import annotations

import importlib.util
from pathlib import Path
import sqlite3
from typing import Any

HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location(
    "runtime_discovery_auto_base", HERE / "runtime_discovery_auto.py"
)
assert SPEC is not None and SPEC.loader is not None
runtime = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runtime)


def launch_declarations(
    db: sqlite3.Connection, package_id: int
) -> tuple[list[str], list[dict[str, Any]]]:
    """Return concrete argv values without inventing missing-argument semantics."""
    arguments: list[str] = []
    if runtime._table_exists(db, "package_arguments"):
        for row in db.execute(
            "SELECT position,argument_value FROM package_arguments "
            "WHERE package_id=? ORDER BY position",
            (package_id,),
        ):
            value = row[1]
            if value is None:
                # The catalog does not yet retain packageArguments.isRequired.
                # Absence of a value alone must not block a runtime observation.
                continue
            text = str(value)
            if len(text.encode("utf-8")) > 4096:
                runtime.base.fail("declared argument exceeded limit")
            arguments.append(text)

    environment: list[dict[str, Any]] = []
    if runtime._table_exists(db, "package_environment"):
        for row in db.execute(
            "SELECT position,name,required,description FROM package_environment "
            "WHERE package_id=? ORDER BY position",
            (package_id,),
        ):
            name = str(row[1] or "").strip()
            required = bool(row[2])
            if not name:
                runtime.base.fail("declared environment variable has no name")
            environment.append({"name": name, "required": required})

    missing_environment = [item["name"] for item in environment if item["required"]]
    if missing_environment:
        runtime.base.fail(
            "required environment unavailable: " + ", ".join(missing_environment[:16])
        )
    return arguments, environment


runtime.launch_declarations = launch_declarations


if __name__ == "__main__":
    raise SystemExit(runtime.main())
