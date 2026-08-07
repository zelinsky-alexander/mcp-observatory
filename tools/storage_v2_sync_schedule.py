#!/usr/bin/env python3
"""Synchronize the Storage v2 static-analysis schedule without executing work."""

from __future__ import annotations

import argparse
import importlib.util
import json
from pathlib import Path


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--database", required=True, type=Path)
    parser.add_argument("--rules", required=True, type=Path)
    parser.add_argument("--analysis-type", default="npm_package_static_v1")
    parser.add_argument("--analyzer-name", default="mcp-observatory-static")
    parser.add_argument("--analyzer-version", default="1.1.0")
    parser.add_argument("--stale-running-after-seconds", type=int, default=3600)
    args = parser.parse_args()

    here = Path(__file__).resolve().parent
    scheduler = load_module("bulk_static_analysis_for_v2_sync", here / "bulk_static_analysis.py")
    foundation = load_module("storage_v2_foundation_for_sync", here / "storage_v2_foundation.py")
    database = args.database.resolve()
    profile, key = scheduler.load_profile(args)

    with scheduler.writer_lock(database):
        db = scheduler.connect(database)
        try:
            with db:
                scheduler.register_profile(db, profile, key)
                scheduler.synchronize(
                    db, key, profile, args.stale_running_after_seconds
                )
                foundation.install(db)
                foundation.refresh_coverage(db)
            result = scheduler.summary(db, key)
        finally:
            db.close()

    result["executed_artifacts"] = 0
    result["mode"] = "synchronize-only"
    print(json.dumps(result, sort_keys=True, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
