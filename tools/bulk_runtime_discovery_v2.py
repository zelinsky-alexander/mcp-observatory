#!/usr/bin/env python3
"""Run bounded local and declared-remote runtime discovery against Storage v2.

Local package observations remain authoritative in the existing runtime tables.
Registry-declared remote URLs are probed in a separate bounded schedule during the
same runtime service invocation. One overall deadline reserves time for publishing
and verification, and remote failures do not discard successful local work.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys
import time


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
    parser.add_argument("--overall-maximum-run-seconds", type=int, default=3150)
    parser.add_argument("--publication-reserve-seconds", type=int, default=120)
    return parser.parse_args()


def _remaining_budget(started: float, overall: int, reserve: int) -> int:
    elapsed = time.monotonic() - started
    return max(0, int(overall - reserve - elapsed))


def main() -> int:
    args = parse_args()
    if args.overall_maximum_run_seconds < 300:
        raise ValueError("overall runtime budget must be at least 300 seconds")
    if args.publication_reserve_seconds < 30:
        raise ValueError("publication reserve must be at least 30 seconds")
    if args.publication_reserve_seconds >= args.overall_maximum_run_seconds:
        raise ValueError("publication reserve must be smaller than overall runtime budget")

    here = Path(__file__).resolve().parent
    scheduler = here / "bulk_runtime_discovery_argument_semantics.py"
    runtime_runner = here / "runtime_discovery_argument_semantics.py"
    remote_scheduler = here / "bulk_remote_runtime_discovery.py"
    remote_runner = here / "remote_runtime_discovery.py"
    mvp = here / "storage_v2_mvp.py"
    runtime_publish = here / "storage_v2_runtime_publish.py"
    started = time.monotonic()

    # Reserve the configured remote allowance plus publication headroom before
    # starting local work. If the local queue is slow, the local scheduler stops
    # at this smaller budget rather than consuming the whole systemd window.
    local_ceiling = (
        args.overall_maximum_run_seconds
        - args.publication_reserve_seconds
        - args.remote_maximum_run_seconds
    )
    local_budget = min(args.maximum_run_seconds, max(1, local_ceiling))
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
            str(local_budget),
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
        remaining = _remaining_budget(
            started,
            args.overall_maximum_run_seconds,
            args.publication_reserve_seconds,
        )
        remote_budget = min(args.remote_maximum_run_seconds, remaining)
        if remote_budget <= 0:
            remote_result = {
                "enabled": True,
                "skipped": True,
                "reason": "shared_runtime_budget_exhausted",
            }
        else:
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
                    str(remote_budget),
                    "--maximum-attempts",
                    str(args.maximum_attempts),
                    "--retry-failed-after-seconds",
                    str(args.retry_failed_after_seconds),
                    "--stale-running-after-seconds",
                    str(args.stale_running_after_seconds),
                    "--phase-timeout-seconds",
                    str(args.remote_phase_timeout_seconds),
                    "--child-timeout-seconds",
                    str(args.remote_child_timeout_seconds),
                ]
            )
            if remote_run.returncode == 0:
                try:
                    remote_result = {"enabled": True, **json.loads(remote_run.stdout)}
                except json.JSONDecodeError as exc:
                    remote_result = {
                        "enabled": True,
                        "error": "remote_scheduler_invalid_output",
                        "detail": str(exc),
                    }
            else:
                # Remote probing is supplementary. Persisted local observations
                # must still be published even if the remote scheduler itself
                # encounters an operational failure.
                remote_result = {
                    "enabled": True,
                    "error": "remote_scheduler_failed",
                    "returncode": remote_run.returncode,
                    "detail": remote_run.stderr[-2048:],
                }
                if remote_run.stderr:
                    sys.stderr.write(remote_run.stderr)

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
                "runtime_service": {
                    "overall_budget_seconds": args.overall_maximum_run_seconds,
                    "local_budget_seconds": local_budget,
                    "publication_reserve_seconds": args.publication_reserve_seconds,
                    "elapsed_seconds": round(time.monotonic() - started, 3),
                },
            },
            sort_keys=True,
            separators=(",", ":"),
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
