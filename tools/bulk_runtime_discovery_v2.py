#!/usr/bin/env python3
"""Run bounded local and declared-remote runtime discovery against Storage v2.

Local package observations remain authoritative in the existing runtime tables.
Registry-declared remote URLs are probed in a separate bounded schedule during the
same runtime batch, then both read models are published to the hot catalog.
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
    parser.add_argument("--guard-binary", required=True, type=Path)
    parser.add_argument("--probe-profile", required=True, type=Path)
    parser.add_argument("--remote-probe-profile", type=Path)
    parser.add_argument("--evidence-root", required=True, type=Path)
    parser.add_argument("--runtime-image", default="auto-node-v1")
    parser.add_argument("--batch-size", type=int, default=10)
    parser.add_argument("--maximum-run-seconds", type=int, default=3000)
    parser.add_argument("--maximum-attempts", type=int, default=3)
    parser.add_argument("--retry-failed-after-seconds", type=int, default=86400)
    parser.add_argument("--stale-running-after-seconds", type=int, default=7200)
    parser.add_argument("--phase-timeout-seconds", type=int, default=180)
    parser.add_argument("--child-timeout-seconds", type=int, default=720)
    parser.add_argument("--remote-batch-size", type=int, default=5)
    parser.add_argument("--remote-maximum-run-seconds", type=int, default=300)
    parser.add_argument("--remote-phase-timeout-seconds", type=int, default=15)
    parser.add_argument("--remote-child-timeout-seconds", type=int, default=45)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    here = Path(__file__).resolve().parent
    scheduler = here / "bulk_runtime_discovery_argument_semantics.py"
    runtime_runner = here / "runtime_discovery_argument_semantics.py"
    remote_scheduler = here / "bulk_remote_runtime_discovery.py"
    remote_runner = here / "remote_runtime_discovery.py"
    mvp = here / "storage_v2_mvp.py"
    runtime_publish = here / "storage_v2_runtime_publish.py"

    scheduler_run = run_checked(
        [
            sys.executable,
            str(scheduler),
            "--database",
            str(args.history_database),
            "--runtime-runner",
            str(runtime_runner),
            "--guard-binary",
            str(args.guard_binary),
            "--probe-profile",
            str(args.probe_profile),
            "--evidence-root",
            str(args.evidence_root),
            "--runtime-image",
            args.runtime_image,
            "--batch-size",
            str(args.batch_size),
            "--maximum-run-seconds",
            str(args.maximum_run_seconds),
            "--maximum-attempts",
            str(args.maximum_attempts),
            "--retry-failed-after-seconds",
            str(args.retry_failed_after_seconds),
            "--stale-running-after-seconds",
            str(args.stale_running_after_seconds),
            "--phase-timeout-seconds",
            str(args.phase_timeout_seconds),
            "--child-timeout-seconds",
            str(args.child_timeout_seconds),
            "--format",
            "json",
        ]
    )
    if scheduler_run.returncode != 0:
        sys.stderr.write(scheduler_run.stderr)
        return scheduler_run.returncode

    remote_result: dict[str, object] = {"enabled": False}
    if args.remote_probe_profile is not None:
        remote_run = run_checked(
            [
                sys.executable,
                str(remote_scheduler),
                "--database",
                str(args.history_database),
                "--remote-runner",
                str(remote_runner),
                "--probe-profile",
                str(args.remote_probe_profile),
                "--batch-size",
                str(args.remote_batch_size),
                "--maximum-run-seconds",
                str(args.remote_maximum_run_seconds),
                "--maximum-attempts",
                str(args.maximum_attempts),
                "--retry-failed-after-seconds",
                str(args.retry_failed_after_seconds),
                "--phase-timeout-seconds",
                str(args.remote_phase_timeout_seconds),
                "--child-timeout-seconds",
                str(args.remote_child_timeout_seconds),
            ]
        )
        if remote_run.returncode != 0:
            sys.stderr.write(remote_run.stderr)
            return remote_run.returncode
        remote_result = {"enabled": True, **json.loads(remote_run.stdout)}

    published = run_checked(
        [
            sys.executable,
            str(mvp),
            "publish",
            "--history",
            str(args.history_database),
            "--hot",
            str(args.hot_database),
        ]
    )
    if published.returncode != 0:
        sys.stderr.write(published.stderr)
        return published.returncode

    runtime_model = run_checked(
        [
            sys.executable,
            str(runtime_publish),
            "--history",
            str(args.history_database),
            "--hot",
            str(args.hot_database),
        ]
    )
    if runtime_model.returncode != 0:
        sys.stderr.write(runtime_model.stderr)
        return runtime_model.returncode

    verified = run_checked(
        [
            sys.executable,
            str(mvp),
            "verify",
            "--history",
            str(args.history_database),
            "--hot",
            str(args.hot_database),
        ]
    )
    if verified.returncode != 0:
        sys.stderr.write(verified.stderr)
        return verified.returncode

    print(
        json.dumps(
            {
                "scheduler": json.loads(scheduler_run.stdout),
                "remote_scheduler": remote_result,
                "published": json.loads(published.stdout),
                "runtime_read_model": json.loads(runtime_model.stdout),
                "verified": json.loads(verified.stdout),
            },
            sort_keys=True,
            separators=(",", ":"),
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
