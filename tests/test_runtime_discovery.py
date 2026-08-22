#!/usr/bin/env python3
"""Offline contract tests for runtime discovery v1."""

import importlib.util
import os
import pathlib
import sqlite3
import subprocess
import sys
import tempfile
import time


ROOT = pathlib.Path(__file__).resolve().parent.parent
SPEC = importlib.util.spec_from_file_location(
    "runtime_discovery", ROOT / "tools" / "runtime_discovery.py"
)
assert SPEC is not None and SPEC.loader is not None
runtime_discovery = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runtime_discovery)


def require_raises(message, function, *args, **kwargs):
    try:
        function(*args, **kwargs)
    except RuntimeError:
        return
    raise AssertionError(message)


def test_bounded_process() -> None:
    completed = runtime_discovery.run(
        [sys.executable, "-c", "import sys; print('ok'); print('note', file=sys.stderr)"],
        timeout=5,
        maximum_output=128,
    )
    assert completed.returncode == 0
    assert completed.stdout == b"ok\n"
    assert completed.stderr == b"note\n"
    require_raises(
        "excessive child output was accepted",
        runtime_discovery.run,
        [sys.executable, "-c", "print('x' * 10000)"],
        timeout=5,
        maximum_output=128,
    )
    started = time.monotonic()
    require_raises(
        "child timeout was not enforced",
        runtime_discovery.run,
        [sys.executable, "-c", "import time; time.sleep(30)"],
        timeout=1,
        maximum_output=128,
    )
    assert time.monotonic() - started < 6


def test_inventory_validation() -> None:
    runtime_discovery.validate_inventory(
        {"inventory_version": 1, "tools": [{"name": "read", "inputSchema": {}}]}
    )
    require_raises(
        "duplicate tool names were accepted",
        runtime_discovery.validate_inventory,
        {"inventory_version": 1, "tools": [{"name": "x"}, {"name": "x"}]},
    )
    require_raises(
        "excessive tool count was accepted",
        runtime_discovery.validate_inventory,
        {"inventory_version": 1, "tools": [{"name": str(i)} for i in range(257)]},
    )


def test_runtime_work_directories_ignore_umask() -> None:
    old_umask = os.umask(0o022)
    try:
        with tempfile.TemporaryDirectory(prefix="mcpo-runtime-permissions-") as temporary:
            root = pathlib.Path(temporary)
            cache = root / "cache"
            work = root / "work"
            runtime_discovery.prepare_writable_directory(cache)
            runtime_discovery.prepare_writable_directory(work)
            assert cache.stat().st_mode & 0o777 == 0o777
            assert work.stat().st_mode & 0o777 == 0o777
    finally:
        os.umask(old_umask)


def test_runtime_install_uses_verified_artifact_and_offline_boundary() -> None:
    expected_user = f"{os.getuid()}:{os.getgid()}"
    captured = []
    original = runtime_discovery.run_docker

    def fake_run_docker(argv, *, timeout, container_id_file):
        captured.append(list(argv))
        return subprocess.CompletedProcess(argv, 0, b"", b"")

    runtime_discovery.run_docker = fake_run_docker
    try:
        with tempfile.TemporaryDirectory(prefix="mcpo-runtime-user-") as temporary:
            root = pathlib.Path(temporary)
            cache = root / "cache"
            work = root / "work"
            artifact = root / "artifact.tgz"
            runtime_discovery.prepare_writable_directory(cache)
            runtime_discovery.prepare_writable_directory(work)
            artifact.write_bytes(b"artifact")
            runtime_discovery.populate_cache("node:test", cache, artifact, 5)
            runtime_discovery.offline_install("node:test", cache, work, artifact, 5)
    finally:
        runtime_discovery.run_docker = original

    assert len(captured) == 2
    cache_argv, install_argv = captured
    for argv in captured:
        user_index = argv.index("--user")
        assert argv[user_index + 1] == expected_user
        assert "/artifact.tgz" in argv
        assert any(
            item.endswith(",dst=/artifact.tgz,ro=true") for item in argv
            if item.startswith("type=bind,")
        )
        assert "--ignore-scripts" in argv

    assert "install" in cache_argv
    assert "--network" not in cache_argv
    assert "--offline" not in cache_argv
    assert "none" not in cache_argv
    assert "--network" in install_argv
    network_index = install_argv.index("--network")
    assert install_argv[network_index + 1] == "none"
    assert "--offline" in install_argv
    assert "--no-save" in install_argv
    assert any(
        item.endswith(",dst=/npm-cache") for item in install_argv
        if item.startswith("type=bind,")
    )
    assert not any(
        item.endswith(",dst=/npm-cache,ro=true") for item in install_argv
        if item.startswith("type=bind,")
    )


def test_persistence_contract() -> None:
    with tempfile.TemporaryDirectory(prefix="mcpo-runtime-test-") as temporary:
        root = pathlib.Path(temporary)
        database = root / "catalog.sqlite"
        connection = sqlite3.connect(database)
        connection.executescript(
            "CREATE TABLE server_versions(id INTEGER PRIMARY KEY);"
            "CREATE TABLE packages(id INTEGER PRIMARY KEY);"
            "INSERT INTO server_versions VALUES(1);"
            "INSERT INTO packages VALUES(10);"
            + runtime_discovery.SCHEMA
        )
        connection.row_factory = sqlite3.Row
        row = connection.execute(
            "SELECT 1 server_version_id, 10 package_id"
        ).fetchone()
        run_id = runtime_discovery.persist(
            connection,
            row,
            "a" * 64,
            "b" * 64,
            "node:test",
            "sha256:" + "c" * 64,
            {"inventory_version": 1, "tools": [{"name": "read"}]},
            root / "evidence",
        )
        stored = connection.execute(
            "SELECT status,guard_version,inventory_sha256 FROM runtime_observation_runs WHERE id=?",
            (run_id,),
        ).fetchone()
        assert stored["status"] == "completed"
        assert stored["guard_version"] == "sha256:" + "c" * 64
        assert len(stored["inventory_sha256"]) == 64
        assert connection.execute(
            "SELECT COUNT(*) FROM runtime_observation_tools WHERE run_id=?", (run_id,)
        ).fetchone()[0] == 1
        connection.close()


def main() -> None:
    test_bounded_process()
    test_inventory_validation()
    test_runtime_work_directories_ignore_umask()
    test_runtime_install_uses_verified_artifact_and_offline_boundary()
    test_persistence_contract()
    print("runtime discovery tests passed")


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, OSError, sqlite3.Error, subprocess.SubprocessError) as exc:
        print(f"runtime discovery test failed: {exc}", file=sys.stderr)
        raise SystemExit(1)
