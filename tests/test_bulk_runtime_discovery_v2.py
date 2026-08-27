#!/usr/bin/env python3
from __future__ import annotations

import argparse
from contextlib import redirect_stdout
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
SPEC = importlib.util.spec_from_file_location(
    "bulk_runtime_discovery_v2", ROOT / "tools" / "bulk_runtime_discovery_v2.py"
)
assert SPEC is not None and SPEC.loader is not None
orchestrator = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(orchestrator)


def completed(argv: list[str], code: int, stdout: str = "{}", stderr: str = ""):
    return subprocess.CompletedProcess(argv, code, stdout, stderr)


class BulkRuntimeDiscoveryV2Tests(unittest.TestCase):
    def args(self) -> argparse.Namespace:
        return argparse.Namespace(
            history_database=Path("history.sqlite"),
            hot_database=Path("hot.sqlite"),
            guard_binary=Path("guard"),
            probe_profile=Path("local-profile.json"),
            remote_probe_profile=Path("remote-profile.json"),
            evidence_root=Path("evidence"),
            runtime_image="auto-node-v1",
            batch_size=25,
            maximum_run_seconds=3000,
            maximum_attempts=3,
            retry_failed_after_seconds=86400,
            stale_running_after_seconds=7200,
            phase_timeout_seconds=180,
            child_timeout_seconds=720,
            remote_batch_size=5,
            remote_maximum_run_seconds=300,
            remote_phase_timeout_seconds=15,
            remote_child_timeout_seconds=45,
            overall_maximum_run_seconds=3150,
            publication_reserve_seconds=120,
        )

    def test_remote_failure_does_not_prevent_publish_and_verify(self) -> None:
        calls: list[list[str]] = []

        def run(argv: list[str]):
            calls.append(argv)
            index = len(calls)
            if index == 1:
                return completed(argv, 0, '{"processed_in_batch":25}')
            if index == 2:
                return completed(argv, 2, "", "remote scheduler exploded")
            return completed(argv, 0, "{}")

        output = io.StringIO()
        with mock.patch.object(orchestrator, "parse_args", return_value=self.args()), \
             mock.patch.object(orchestrator, "run_checked", side_effect=run), \
             mock.patch.object(orchestrator.time, "monotonic", return_value=100.0), \
             redirect_stdout(output):
            rc = orchestrator.main()

        self.assertEqual(rc, 0)
        self.assertEqual(len(calls), 5)
        local = calls[0]
        local_budget = local[local.index("--maximum-run-seconds") + 1]
        self.assertEqual(local_budget, "2730")
        result = json.loads(output.getvalue())
        self.assertEqual(
            result["remote_scheduler"]["error"], "remote_scheduler_failed"
        )
        self.assertEqual(result["runtime_service"]["local_budget_seconds"], 2730)

    def test_remote_budget_is_capped_by_shared_deadline(self) -> None:
        args = self.args()
        calls: list[list[str]] = []
        times = iter([100.0, 3070.0, 3070.0])

        def run(argv: list[str]):
            calls.append(argv)
            return completed(argv, 0, "{}")

        with mock.patch.object(orchestrator, "parse_args", return_value=args), \
             mock.patch.object(orchestrator, "run_checked", side_effect=run), \
             mock.patch.object(orchestrator.time, "monotonic", side_effect=lambda: next(times)), \
             redirect_stdout(io.StringIO()):
            rc = orchestrator.main()

        self.assertEqual(rc, 0)
        remote = calls[1]
        remote_budget = remote[remote.index("--maximum-run-seconds") + 1]
        self.assertEqual(remote_budget, "60")


if __name__ == "__main__":
    unittest.main()
