#!/usr/bin/env python3
"""Run bounded static analysis against the v2 history store and publish summaries.

This is the side-by-side MVP writer path. The existing analyzer writes detailed
v1 evidence only to the history/control database and the dedicated v2 evidence
root. Compact summaries and content-addressed evidence-manifest references are
then copied to the hot catalog. The hot catalog never receives v1 detail rows.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys


def run_checked(argv: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        argv,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--history-database", required=True, type=Path)
    parser.add_argument("--hot-database", required=True, type=Path)
    parser.add_argument("--observatory-binary", required=True, type=Path)
    parser.add_argument("--rules", required=True, type=Path)
    parser.add_argument("--evidence-root", required=True, type=Path)
    parser.add_argument("--batch-size", type=int, default=1)
    parser.add_argument("--maximum-run-seconds", type=int, default=900)
    parser.add_argument("--child-timeout-seconds", type=int, default=300)
    parser.add_argument("--maximum-attempts", type=int, default=3)
    parser.add_argument("--retry-failed-after-seconds", type=int, default=86400)
    parser.add_argument("--npm-registry-url")
    parser.add_argument("--pypi-registry-url")
    parser.add_argument(
        "--bundle-limit",
        type=int,
        default=0,
        help="optionally bundle up to N evidence directories",
    )
    parser.add_argument("--bundle-root", type=Path)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    here = Path(__file__).resolve().parent
    scheduler = here / "bulk_static_analysis.py"
    foundation = here / "storage_v2_foundation.py"
    mvp = here / "storage_v2_mvp.py"
    if args.batch_size < 1 or args.batch_size > 1000:
        raise ValueError("batch size must be between 1 and 1000")

    init = run_checked([
        sys.executable,
        str(foundation),
        "--database",
        str(args.history_database),
        "--refresh-coverage",
    ])
    if init.returncode != 0:
        sys.stderr.write(init.stderr)
        return init.returncode

    argv = [
        sys.executable,
        str(scheduler),
        "--database",
        str(args.history_database),
        "--observatory-binary",
        str(args.observatory_binary),
        "--rules",
        str(args.rules),
        "--evidence-root",
        str(args.evidence_root),
        "--batch-size",
        str(args.batch_size),
        "--maximum-run-seconds",
        str(args.maximum_run_seconds),
        "--child-timeout-seconds",
        str(args.child_timeout_seconds),
        "--maximum-attempts",
        str(args.maximum_attempts),
        "--retry-failed-after-seconds",
        str(args.retry_failed_after_seconds),
        "--format",
        "json",
    ]
    if args.npm_registry_url:
        argv += ["--npm-registry-url", args.npm_registry_url]
    if args.pypi_registry_url:
        argv += ["--pypi-registry-url", args.pypi_registry_url]

    child = run_checked(argv)
    if child.returncode != 0:
        sys.stderr.write(child.stderr)
        return child.returncode

    materialize = run_checked([
        sys.executable,
        str(foundation),
        "--database",
        str(args.history_database),
        "--backfill-batch-size",
        str(max(100, args.batch_size * 4)),
        "--refresh-coverage",
    ])
    if materialize.returncode != 0:
        sys.stderr.write(materialize.stderr)
        return materialize.returncode

    bundle_result = None
    if args.bundle_limit:
        if args.bundle_root is None:
            raise ValueError("--bundle-root is required when --bundle-limit is non-zero")
        bundled = run_checked([
            sys.executable,
            str(mvp),
            "bundle-evidence",
            "--source-root",
            str(args.evidence_root),
            "--destination-root",
            str(args.bundle_root),
            "--history-database",
            str(args.history_database),
            "--limit",
            str(args.bundle_limit),
        ])
        if bundled.returncode != 0:
            sys.stderr.write(bundled.stderr)
            return bundled.returncode
        bundle_result = json.loads(bundled.stdout)

    # Publish after bundling so evidence manifests and all materialized counters
    # reach the compact hot database in the same visible generation.
    publish = run_checked([
        sys.executable,
        str(mvp),
        "publish",
        "--history",
        str(args.history_database),
        "--hot",
        str(args.hot_database),
    ])
    if publish.returncode != 0:
        sys.stderr.write(publish.stderr)
        return publish.returncode

    result = {
        "scheduler": json.loads(child.stdout),
        "materialize": materialize.stdout.strip(),
        "published": json.loads(publish.stdout),
        "bundles": bundle_result,
    }
    print(json.dumps(result, sort_keys=True, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
