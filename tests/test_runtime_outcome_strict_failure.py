#!/usr/bin/env python3
from __future__ import annotations

import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parent.parent
SPEC = importlib.util.spec_from_file_location(
    "bulk_runtime_discovery_auto_strict", ROOT / "tools" / "bulk_runtime_discovery_auto.py"
)
assert SPEC is not None and SPEC.loader is not None
scheduler = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(scheduler)


class StrictRuntimeFailureSemanticsTests(unittest.TestCase):
    def test_unknown_guard_stage_is_inconclusive(self) -> None:
        state, code, _ = scheduler.classify_child_failure(
            "runtime discovery failed: mcp-native-guard inspect failed: FAIL mystery\n",
            1,
        )
        self.assertEqual((state, code), ("inconclusive", "protocol_stage_unknown"))

    def test_install_failure_is_inconclusive(self) -> None:
        state, code, _ = scheduler.classify_child_failure(
            "runtime discovery failed: offline install failed: npm error", 2
        )
        self.assertEqual((state, code), ("inconclusive", "runtime_install_failed"))

    def test_generic_harness_failure_is_inconclusive(self) -> None:
        state, code, _ = scheduler.classify_child_failure(
            "runtime discovery failed: child process exceeded 180 seconds", 2
        )
        self.assertEqual((state, code), ("inconclusive", "runtime_harness_failed"))

    def test_unsupported_node_runtime_is_unresolvable(self) -> None:
        state, code, _ = scheduler.classify_child_failure(
            "runtime discovery failed: no approved Node runtime satisfies engines.node='<=18' and Guard compatibility",
            2,
        )
        self.assertEqual((state, code), ("unresolvable", "unsupported_runtime"))

    def test_malformed_json_remains_true_protocol_failure(self) -> None:
        state, code, _ = scheduler.classify_child_failure(
            "runtime discovery failed: mcp-native-guard inspect failed: FAIL malformed_json\n",
            1,
        )
        self.assertEqual((state, code), ("failed", "protocol_malformed_json"))


if __name__ == "__main__":
    unittest.main()
