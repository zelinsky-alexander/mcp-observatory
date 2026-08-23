#!/usr/bin/env python3
from __future__ import annotations

import importlib.util
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
SPEC = importlib.util.spec_from_file_location(
    "runtime_discovery_auto", ROOT / "tools" / "runtime_discovery_auto.py"
)
assert SPEC is not None and SPEC.loader is not None
auto = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(auto)


def test_default_runtime_prefers_node22() -> None:
    original = auto.base.run_docker
    seen: list[list[str]] = []

    def fake(argv, *, timeout, container_id_file):
        seen.append(list(argv))
        return subprocess.CompletedProcess(argv, 0, b"", b"")

    auto.base.run_docker = fake
    try:
        with tempfile.TemporaryDirectory() as temporary:
            guard = Path(temporary) / "guard"
            guard.write_bytes(b"guard")
            image = auto.resolve_runtime_image(
                auto.AUTO_RUNTIME_POLICY, None, guard, Path(temporary), 5
            )
    finally:
        auto.base.run_docker = original
    assert image == "node:22-trixie-slim"
    assert any("node:22-trixie-slim" in argv for argv in seen)


def test_engine_range_selects_highest_satisfying_compatible_runtime() -> None:
    original = auto.base.run_docker

    def fake(argv, *, timeout, container_id_file):
        image = argv[argv.index("--cpus") + 2]
        if "node" in argv and "-e" in argv:
            # Simulate Node 24 not satisfying <23, Node 22 satisfying it.
            code = 42 if image == "node:24-trixie-slim" else 0
            return subprocess.CompletedProcess(argv, code, b"", b"")
        return subprocess.CompletedProcess(argv, 0, b"", b"")

    auto.base.run_docker = fake
    try:
        with tempfile.TemporaryDirectory() as temporary:
            guard = Path(temporary) / "guard"
            guard.write_bytes(b"guard")
            image = auto.resolve_runtime_image(
                auto.AUTO_RUNTIME_POLICY, ">=18 <23", guard, Path(temporary), 5
            )
    finally:
        auto.base.run_docker = original
    assert image == "node:22-trixie-slim"


def test_fixed_image_remains_supported() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        guard = Path(temporary) / "guard"
        guard.write_bytes(b"guard")
        assert auto.resolve_runtime_image(
            "node:22-trixie-slim", ">=18", guard, Path(temporary), 5
        ) == "node:22-trixie-slim"


if __name__ == "__main__":
    test_default_runtime_prefers_node22()
    test_engine_range_selects_highest_satisfying_compatible_runtime()
    test_fixed_image_remains_supported()
    print("automatic runtime resolver tests passed")
