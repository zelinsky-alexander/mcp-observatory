#!/usr/bin/env python3
"""Contained npm MCP runtime discovery and version-to-version tool drift.

This dependency-free MVP deliberately supports exact npm versions and stdio only.
It never invokes tools. The host verifies npm metadata and the exact artifact, a
disposable container populates the package cache, and installation plus runtime
discovery run with --network none.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import re
import selectors
import signal
import sqlite3
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.parse
import urllib.request
from typing import Any

SCHEMA = """
CREATE TABLE IF NOT EXISTS runtime_observation_runs(
  id INTEGER PRIMARY KEY,
  server_version_id INTEGER NOT NULL REFERENCES server_versions(id) ON DELETE RESTRICT,
  package_id INTEGER NOT NULL REFERENCES packages(id) ON DELETE RESTRICT,
  status TEXT NOT NULL CHECK(status IN ('running','completed','failed')),
  artifact_sha256 TEXT,
  launch_profile_sha256 TEXT,
  sandbox_image TEXT NOT NULL,
  guard_version TEXT NOT NULL,
  inventory_sha256 TEXT,
  inventory_json TEXT,
  started_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
  completed_at TEXT,
  error_stage TEXT,
  error_message TEXT
);
CREATE INDEX IF NOT EXISTS runtime_observation_lookup
ON runtime_observation_runs(package_id,status,id);
CREATE TABLE IF NOT EXISTS runtime_observation_tools(
  run_id INTEGER NOT NULL REFERENCES runtime_observation_runs(id) ON DELETE CASCADE,
  name TEXT NOT NULL,
  definition_json TEXT NOT NULL,
  definition_sha256 TEXT NOT NULL,
  PRIMARY KEY(run_id,name)
);
"""


def fail(message: str) -> "NoReturn":
    raise RuntimeError(message)


def canonical(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"))


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def run(
    argv: list[str],
    *,
    timeout: int,
    maximum_output: int = 1_048_576,
    container_id_file: Path | None = None,
) -> subprocess.CompletedProcess[bytes]:
    if timeout <= 0 or maximum_output <= 0:
        fail("child timeout and output limit must be positive")
    process = subprocess.Popen(
        argv,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        close_fds=True,
        start_new_session=True,
        env={"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "LANG": "C.UTF-8"},
    )
    assert process.stdout is not None and process.stderr is not None
    selector = selectors.DefaultSelector()
    selector.register(process.stdout, selectors.EVENT_READ, "stdout")
    selector.register(process.stderr, selectors.EVENT_READ, "stderr")
    output = {"stdout": bytearray(), "stderr": bytearray()}
    deadline = time.monotonic() + timeout
    failure = ""
    try:
        while selector.get_map() or process.poll() is None:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                failure = f"child process exceeded {timeout} seconds"
                break
            for key, _ in selector.select(timeout=min(0.25, remaining)):
                chunk = os.read(key.fileobj.fileno(), 8192)
                if not chunk:
                    selector.unregister(key.fileobj)
                    key.fileobj.close()
                    continue
                target = output[key.data]
                if len(target) + len(chunk) > maximum_output:
                    failure = "child output exceeded configured limit"
                    break
                target.extend(chunk)
            if failure:
                break
        if failure:
            _terminate_group(process)
            if container_id_file is not None:
                _remove_container(container_id_file)
            fail(failure)
        if process.poll() is None:
            process.wait(timeout=3)
    finally:
        selector.close()
        for stream in (process.stdout, process.stderr):
            if not stream.closed:
                stream.close()
        if container_id_file is not None:
            container_id_file.unlink(missing_ok=True)
    return subprocess.CompletedProcess(
        argv, process.returncode, bytes(output["stdout"]), bytes(output["stderr"])
    )


def _terminate_group(process: subprocess.Popen[bytes]) -> None:
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    try:
        process.wait(timeout=3)
        return
    except subprocess.TimeoutExpired:
        pass
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    process.wait(timeout=3)


def _remove_container(container_id_file: Path) -> None:
    try:
        container_id = container_id_file.read_text(encoding="ascii")[:128].strip()
    except OSError:
        return
    if not re.fullmatch(r"[0-9a-f]{12,64}", container_id):
        return
    try:
        subprocess.run(
            ["docker", "rm", "-f", container_id],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            timeout=15,
            check=False,
            env={"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "LANG": "C.UTF-8"},
        )
    except (OSError, subprocess.TimeoutExpired):
        pass


def run_docker(
    argv: list[str], *, timeout: int, container_id_file: Path
) -> subprocess.CompletedProcess[bytes]:
    if argv[:2] != ["docker", "run"]:
        fail("internal Docker argument vector is invalid")
    container_id_file.unlink(missing_ok=True)
    return run(
        [*argv[:2], "--cidfile", str(container_id_file), *argv[2:]],
        timeout=timeout,
        container_id_file=container_id_file,
    )


def resolve_package(db: sqlite3.Connection, server: str, version: str, package: str) -> sqlite3.Row:
    db.row_factory = sqlite3.Row
    rows = db.execute(
        """
        SELECT p.id package_id,p.server_version_id,sv.server_identifier,sv.server_version,
               p.registry_type,p.identifier package_identifier,p.version package_version,p.transport
        FROM packages p JOIN server_versions sv ON sv.id=p.server_version_id
        WHERE sv.server_identifier=? AND sv.server_version=? AND p.identifier=?
        ORDER BY p.position,p.id
        """,
        (server, version, package),
    ).fetchall()
    if len(rows) != 1:
        fail("exact package selection is missing or ambiguous")
    row = rows[0]
    if row["registry_type"] != "npm" or row["transport"] != "stdio":
        fail("runtime-discovery-v1 supports exact npm stdio packages only")
    if not row["package_version"]:
        fail("exact npm package version is required")
    return row


def npm_metadata(package: str, version: str) -> tuple[dict[str, Any], bytes]:
    encoded = urllib.parse.quote(package, safe="")
    url = f"https://registry.npmjs.org/{encoded}/{urllib.parse.quote(version, safe='')}"
    request = urllib.request.Request(url, headers={"Accept": "application/json", "User-Agent": "mcp-observatory-runtime-discovery/1"})
    with urllib.request.urlopen(request, timeout=60) as response:
        body = response.read(2 * 1024 * 1024 + 1)
    if len(body) > 2 * 1024 * 1024:
        fail("npm metadata exceeded limit")
    value = json.loads(body)
    if value.get("name") != package or value.get("version") != version:
        fail("npm metadata identity mismatch")
    return value, body


def download_artifact(metadata: dict[str, Any]) -> tuple[bytes, str]:
    dist = metadata.get("dist")
    if not isinstance(dist, dict):
        fail("npm metadata has no dist object")
    url = dist.get("tarball")
    integrity = dist.get("integrity")
    if not isinstance(url, str) or not isinstance(integrity, str):
        fail("npm metadata lacks tarball or integrity")
    with urllib.request.urlopen(url, timeout=120) as response:
        data = response.read(64 * 1024 * 1024 + 1)
    if len(data) > 64 * 1024 * 1024:
        fail("npm artifact exceeded limit")
    algorithm, separator, encoded = integrity.partition("-")
    if not separator or algorithm not in {"sha256", "sha512"}:
        fail("unsupported npm integrity algorithm")
    digest = hashlib.new(algorithm, data).digest()
    if digest != base64.b64decode(encoded, validate=True):
        fail("npm integrity mismatch")
    return data, integrity


def package_bin(artifact: Path, package: str) -> str:
    with tarfile.open(artifact, "r:gz") as archive:
        member = archive.getmember("package/package.json")
        if member.size > 1024 * 1024:
            fail("package.json exceeded limit")
        stream = archive.extractfile(member)
        if stream is None:
            fail("package.json is not a regular member")
        manifest = json.load(stream)
    entry = manifest.get("bin")
    if isinstance(entry, str):
        return entry
    if isinstance(entry, dict):
        preferred = package.rsplit("/", 1)[-1]
        if isinstance(entry.get(preferred), str):
            return entry[preferred]
        candidates = sorted(value for value in entry.values() if isinstance(value, str))
        if len(candidates) == 1:
            return candidates[0]
    fail("package has no unambiguous executable bin entry")


def docker_base(image: str) -> list[str]:
    return [
        "docker", "run", "--rm", "--cap-drop", "ALL",
        "--security-opt", "no-new-privileges", "--pids-limit", "128",
        "--memory", "512m", "--cpus", "1.0", "--ulimit", "nofile=256:256",
        image,
    ]


def populate_cache(image: str, cache: Path, package: str, version: str, timeout: int) -> None:
    argv = [
        "docker", "run", "--rm", "--cap-drop", "ALL", "--security-opt", "no-new-privileges",
        "--pids-limit", "128", "--memory", "512m", "--cpus", "1.0",
        "--mount", f"type=bind,src={cache},dst=/npm-cache",
        image, "npm", "cache", "add", f"{package}@{version}", "--cache", "/npm-cache",
        "--no-audit", "--no-fund",
    ]
    result = run_docker(argv, timeout=timeout, container_id_file=cache.parent / "cache.cid")
    if result.returncode != 0:
        fail("cache population failed: " + result.stderr.decode("utf-8", "replace")[-2000:])


def offline_install(image: str, cache: Path, work: Path, package: str, version: str, timeout: int) -> None:
    (work / "package.json").write_text(canonical({"private": True, "dependencies": {package: version}}) + "\n", encoding="utf-8")
    argv = [
        "docker", "run", "--rm", "--network", "none", "--cap-drop", "ALL",
        "--security-opt", "no-new-privileges", "--pids-limit", "128", "--memory", "768m",
        "--cpus", "1.0", "--ulimit", "nofile=256:256",
        "--mount", f"type=bind,src={cache},dst=/npm-cache,ro=true",
        "--mount", f"type=bind,src={work},dst=/work", "--workdir", "/work",
        image, "npm", "install", "--offline", "--ignore-scripts", "--omit=dev",
        "--no-audit", "--no-fund", "--cache", "/npm-cache",
    ]
    result = run_docker(argv, timeout=timeout, container_id_file=work.parent / "install.cid")
    if result.returncode != 0:
        fail("offline install failed: " + result.stderr.decode("utf-8", "replace")[-2000:])


def inspect_runtime(image: str, work: Path, guard: Path, package: str, bin_entry: str, timeout: int) -> dict[str, Any]:
    package_path = package
    command = f"/work/node_modules/{package_path}/{bin_entry.lstrip('./')}"
    argv = [
        "docker", "run", "--rm", "--network", "none", "--read-only",
        "--tmpfs", "/tmp:rw,nosuid,nodev,size=67108864", "--user", "65532:65532",
        "--cap-drop", "ALL", "--security-opt", "no-new-privileges", "--pids-limit", "64",
        "--memory", "512m", "--cpus", "1.0", "--ulimit", "nofile=128:128",
        "--mount", f"type=bind,src={work},dst=/work,ro=true",
        "--mount", f"type=bind,src={guard.resolve()},dst=/opt/mcp-native-guard,ro=true",
        image, "/opt/mcp-native-guard", "inspect", "--timeout", str(min(timeout, 300)), "--",
        "node", command,
    ]
    result = run_docker(argv, timeout=timeout + 15, container_id_file=work.parent / "runtime.cid")
    if result.returncode != 0:
        fail("mcp-native-guard inspect failed: " + result.stderr.decode("utf-8", "replace")[-2000:])
    inventory = json.loads(result.stdout)
    validate_inventory(inventory)
    return inventory


def validate_inventory(inventory: Any) -> None:
    if not isinstance(inventory, dict) or inventory.get("inventory_version") != 1:
        fail("guard returned an unsupported inventory")
    tools = inventory.get("tools")
    if not isinstance(tools, list) or len(tools) > 256:
        fail("guard inventory tools are invalid")
    names: set[str] = set()
    for tool in tools:
        if not isinstance(tool, dict) or not isinstance(tool.get("name"), str):
            fail("guard inventory contains an invalid tool")
        if tool["name"] in names:
            fail("guard inventory contains duplicate tool names")
        names.add(tool["name"])


def persist(db: sqlite3.Connection, row: sqlite3.Row, artifact_sha: str, profile_sha: str,
            image: str, guard_version: str, inventory: dict[str, Any], evidence_root: Path) -> int:
    inventory_text = canonical(inventory)
    inventory_sha = sha256_bytes(inventory_text.encode())
    evidence = evidence_root / "runtime" / "sha256" / artifact_sha[:2] / artifact_sha / profile_sha
    evidence.mkdir(parents=True, exist_ok=True)
    (evidence / "inventory.json").write_text(inventory_text + "\n", encoding="utf-8")
    with db:
        cursor = db.execute(
            """INSERT INTO runtime_observation_runs(
            server_version_id,package_id,status,artifact_sha256,launch_profile_sha256,
            sandbox_image,guard_version,inventory_sha256,inventory_json,completed_at)
            VALUES(?,?,'completed',?,?,?,?,?,?,CURRENT_TIMESTAMP)""",
            (row["server_version_id"], row["package_id"], artifact_sha, profile_sha,
             image, guard_version, inventory_sha, inventory_text),
        )
        run_id = int(cursor.lastrowid)
        for tool in inventory["tools"]:
            definition = canonical(tool)
            db.execute(
                "INSERT INTO runtime_observation_tools(run_id,name,definition_json,definition_sha256) VALUES(?,?,?,?)",
                (run_id, tool["name"], definition, sha256_bytes(definition.encode())),
            )
    return run_id


def compare(db: sqlite3.Connection, older: int, newer: int) -> dict[str, Any]:
    def load(run_id: int) -> dict[str, str]:
        return {name: definition for name, definition in db.execute(
            "SELECT name,definition_json FROM runtime_observation_tools WHERE run_id=?", (run_id,))}
    left, right = load(older), load(newer)
    return {
        "older_run_id": older,
        "newer_run_id": newer,
        "added": sorted(right.keys() - left.keys()),
        "removed": sorted(left.keys() - right.keys()),
        "modified": sorted(name for name in left.keys() & right.keys() if left[name] != right[name]),
        "unchanged": sorted(name for name in left.keys() & right.keys() if left[name] == right[name]),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)
    observe = sub.add_parser("observe")
    for name in ("database", "server", "version", "package", "guard-binary", "evidence-root"):
        observe.add_argument("--" + name, required=True)
    observe.add_argument("--runtime-image", default="node:22-bookworm-slim")
    observe.add_argument("--timeout", type=int, default=180)
    drift = sub.add_parser("compare")
    drift.add_argument("--database", required=True)
    drift.add_argument("--older-run-id", type=int, required=True)
    drift.add_argument("--newer-run-id", type=int, required=True)
    args = parser.parse_args()
    db = sqlite3.connect(args.database)
    db.execute("PRAGMA foreign_keys=ON")
    db.executescript(SCHEMA)
    if args.command == "compare":
        print(canonical(compare(db, args.older_run_id, args.newer_run_id)))
        return 0
    row = resolve_package(db, args.server, args.version, args.package)
    metadata, _ = npm_metadata(row["package_identifier"], row["package_version"])
    artifact_bytes, _ = download_artifact(metadata)
    artifact_sha = sha256_bytes(artifact_bytes)
    guard = Path(args.guard_binary).resolve()
    if not guard.is_file():
        fail("guard binary does not exist")
    bin_entry: str
    with tempfile.TemporaryDirectory(prefix="mcpo-runtime-") as temporary:
        root = Path(temporary)
        cache, work = root / "cache", root / "work"
        cache.mkdir(mode=0o777)
        work.mkdir(mode=0o777)
        artifact = root / "artifact.tgz"
        artifact.write_bytes(artifact_bytes)
        bin_entry = package_bin(artifact, row["package_identifier"])
        guard_sha256 = sha256_file(guard)
        profile = {
            "profile_version": 1, "registry": "npm", "transport": "stdio",
            "package": row["package_identifier"], "version": row["package_version"],
            "bin": bin_entry, "install_scripts": False, "runtime_network": "none",
            "image": args.runtime_image, "guard_sha256": guard_sha256,
        }
        profile_sha = sha256_bytes(canonical(profile).encode())
        populate_cache(args.runtime_image, cache, row["package_identifier"], row["package_version"], args.timeout)
        offline_install(args.runtime_image, cache, work, row["package_identifier"], row["package_version"], args.timeout)
        inventory = inspect_runtime(args.runtime_image, work, guard, row["package_identifier"], bin_entry, args.timeout)
        run_id = persist(
            db, row, artifact_sha, profile_sha, args.runtime_image,
            "sha256:" + guard_sha256, inventory, Path(args.evidence_root)
        )
    print(canonical({"status": "completed", "runtime_observation_run_id": run_id,
                     "artifact_sha256": artifact_sha, "launch_profile_sha256": profile_sha,
                     "guard_sha256": guard_sha256,
                     "tool_count": len(inventory["tools"])}))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, OSError, sqlite3.Error, json.JSONDecodeError, subprocess.TimeoutExpired) as exc:
        print(f"runtime discovery failed: {exc}", file=sys.stderr)
        raise SystemExit(2)
