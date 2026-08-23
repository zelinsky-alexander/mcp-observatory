#!/usr/bin/env python3
"""Automatic Node runtime resolver layered over runtime_discovery.py.

The resolver is deterministic and discovery-only. For exact npm stdio artifacts it
reads the package's declared ``engines.node`` range, selects the highest approved
runtime image whose actual Node version satisfies that range, verifies that the
current Native Guard binary can execute in that image, preserves declared launch
arguments, rejects unavailable required environment declarations, captures a bounded
server-stderr diagnostic, and then delegates persistence to the existing runtime
discovery implementation.
"""

from __future__ import annotations

import argparse
import importlib.util
import json
from pathlib import Path
import sqlite3
import subprocess
import sys
import tarfile
import tempfile
from typing import Any

HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location("runtime_discovery_base", HERE / "runtime_discovery.py")
assert SPEC is not None and SPEC.loader is not None
base = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(base)

AUTO_RUNTIME_POLICY = "auto-node-v1"
AUTO_RUNTIME_CANDIDATES: tuple[tuple[int, str], ...] = (
    (24, "node:24-trixie-slim"),
    (22, "node:22-trixie-slim"),
    (20, "node:20-trixie-slim"),
)
AUTO_DEFAULT_IMAGE = "node:22-trixie-slim"
MAX_SERVER_STDERR_BYTES = 4096

SEMVER_SCRIPT = r"""
let semver = null;
for (const p of ['/usr/local/lib/node_modules/npm/node_modules/semver', 'semver']) {
  try { semver = require(p); break; } catch (_) {}
}
if (!semver) process.exit(43);
process.exit(semver.satisfies(process.version, process.argv[1], {loose:true, includePrerelease:true}) ? 0 : 42);
""".strip()

# Keep reading stderr after the capture bound so the server can never block on a
# full stderr pipe. The wrapper preserves stdin/stdout as the MCP transport and
# runs in the same restricted container/process group as the target server.
STDERR_CAPTURE_SCRIPT = r"""
const fs = require('fs');
const {spawn} = require('child_process');
const limit = 4096;
const target = process.argv[1];
const args = process.argv.slice(2);
const fd = fs.openSync('/diagnostics/server.stderr', 'w', 0o644);
let kept = 0;
let closed = false;
function closeFile() {
  if (!closed) { closed = true; try { fs.closeSync(fd); } catch (_) {} }
}
const child = spawn(process.execPath, [target, ...args], {stdio:['inherit','inherit','pipe']});
child.stderr.on('data', chunk => {
  if (kept < limit) {
    const part = chunk.subarray(0, Math.min(chunk.length, limit - kept));
    if (part.length) { fs.writeSync(fd, part); kept += part.length; }
  }
});
child.stderr.on('end', closeFile);
child.on('error', error => {
  const text = Buffer.from(String(error && error.message ? error.message : error));
  if (kept < limit) {
    const part = text.subarray(0, Math.min(text.length, limit - kept));
    if (part.length) fs.writeSync(fd, part);
  }
  closeFile();
  process.exit(127);
});
child.on('close', (code, signal) => {
  closeFile();
  if (signal) { process.kill(process.pid, signal); return; }
  process.exit(code === null ? 1 : code);
});
""".strip()


def _table_exists(db: sqlite3.Connection, name: str) -> bool:
    return db.execute(
        "SELECT 1 FROM sqlite_schema WHERE type='table' AND name=?", (name,)
    ).fetchone() is not None


def package_manifest(artifact: Path) -> dict[str, Any]:
    with tarfile.open(artifact, "r:gz") as archive:
        member = archive.getmember("package/package.json")
        if member.size > 1024 * 1024:
            base.fail("package.json exceeded limit")
        stream = archive.extractfile(member)
        if stream is None:
            base.fail("package.json is not a regular member")
        value = json.load(stream)
    if not isinstance(value, dict):
        base.fail("package.json is not an object")
    return value


def node_engine(manifest: dict[str, Any]) -> str | None:
    engines = manifest.get("engines")
    if not isinstance(engines, dict):
        return None
    value = engines.get("node")
    if not isinstance(value, str) or not value.strip():
        return None
    if len(value) > 256:
        base.fail("engines.node range exceeded limit")
    return value.strip()


def launch_declarations(
    db: sqlite3.Connection, package_id: int
) -> tuple[list[str], list[dict[str, Any]]]:
    """Load the exact Registry-declared argv/environment launch prerequisites."""
    arguments: list[str] = []
    if _table_exists(db, "package_arguments"):
        for row in db.execute(
            "SELECT position,argument_value FROM package_arguments WHERE package_id=? ORDER BY position",
            (package_id,),
        ):
            value = row[1]
            if value is None:
                base.fail("required declared argument has no value")
            text = str(value)
            if len(text.encode("utf-8")) > 4096:
                base.fail("declared argument exceeded limit")
            arguments.append(text)

    environment: list[dict[str, Any]] = []
    if _table_exists(db, "package_environment"):
        for row in db.execute(
            "SELECT position,name,required,description FROM package_environment WHERE package_id=? ORDER BY position",
            (package_id,),
        ):
            name = str(row[1] or "").strip()
            required = bool(row[2])
            if not name:
                base.fail("declared environment variable has no name")
            environment.append({"name": name, "required": required})

    missing = [item["name"] for item in environment if item["required"]]
    if missing:
        base.fail("required environment unavailable: " + ", ".join(missing[:16]))
    return arguments, environment


def _candidate_satisfies(image: str, requirement: str, root: Path, timeout: int) -> bool:
    argv = [
        "docker", "run", "--rm", "--network", "none",
        "--cap-drop", "ALL", "--security-opt", "no-new-privileges",
        "--pids-limit", "32", "--memory", "128m", "--cpus", "0.5",
        image, "node", "-e", SEMVER_SCRIPT, requirement,
    ]
    result = base.run_docker(argv, timeout=timeout, container_id_file=root / "resolve-node.cid")
    if result.returncode == 0:
        return True
    if result.returncode == 42:
        return False
    return False


def _guard_compatible(image: str, guard: Path, root: Path, timeout: int) -> bool:
    argv = [
        "docker", "run", "--rm", "--network", "none", "--read-only",
        "--user", "65532:65532", "--cap-drop", "ALL",
        "--security-opt", "no-new-privileges", "--pids-limit", "32",
        "--memory", "128m", "--cpus", "0.5",
        "--mount", f"type=bind,src={guard.resolve()},dst=/opt/mcp-native-guard,ro=true",
        image, "/opt/mcp-native-guard", "--version",
    ]
    result = base.run_docker(argv, timeout=timeout, container_id_file=root / "resolve-guard.cid")
    return result.returncode == 0


def resolve_runtime_image(
    policy: str,
    requirement: str | None,
    guard: Path,
    root: Path,
    timeout: int,
) -> str:
    if policy != AUTO_RUNTIME_POLICY:
        return policy

    if requirement is None:
        ordered = [AUTO_DEFAULT_IMAGE] + [
            image for _, image in AUTO_RUNTIME_CANDIDATES if image != AUTO_DEFAULT_IMAGE
        ]
        for image in ordered:
            if _guard_compatible(image, guard, root, timeout):
                return image
        base.fail("no approved default Node runtime can execute the current Native Guard")

    for _, image in AUTO_RUNTIME_CANDIDATES:
        if not _candidate_satisfies(image, requirement, root, timeout):
            continue
        if _guard_compatible(image, guard, root, timeout):
            return image
    base.fail(f"no approved Node runtime satisfies engines.node={requirement!r} and Guard compatibility")


def _diagnostic_text(path: Path) -> str:
    try:
        raw = path.read_bytes()[:MAX_SERVER_STDERR_BYTES]
    except OSError:
        return ""
    text = raw.decode("utf-8", "replace")
    return "".join(character if character in "\n\t" or ord(character) >= 0x20 else "?" for character in text)


def inspect_runtime(
    image: str,
    work: Path,
    guard: Path,
    package: str,
    bin_entry: str,
    arguments: list[str],
    timeout: int,
) -> dict[str, Any]:
    package_path = package
    command = f"/work/node_modules/{package_path}/{bin_entry.lstrip('./')}"
    diagnostics = work.parent / "diagnostics"
    base.prepare_writable_directory(diagnostics)
    stderr_path = diagnostics / "server.stderr"
    argv = [
        "docker", "run", "--rm", "--network", "none", "--read-only",
        "--tmpfs", "/tmp:rw,nosuid,nodev,size=67108864", "--user", "65532:65532",
        "--cap-drop", "ALL", "--security-opt", "no-new-privileges", "--pids-limit", "64",
        "--memory", "512m", "--cpus", "1.0", "--ulimit", "nofile=128:128",
        "--mount", f"type=bind,src={work},dst=/work,ro=true",
        "--mount", f"type=bind,src={diagnostics},dst=/diagnostics",
        "--mount", f"type=bind,src={guard.resolve()},dst=/opt/mcp-native-guard,ro=true",
        image, "/opt/mcp-native-guard", "inspect", "--timeout", str(min(timeout, 300)), "--",
        "node", "-e", STDERR_CAPTURE_SCRIPT, command, *arguments,
    ]
    result = base.run_docker(
        argv, timeout=timeout + 15, container_id_file=work.parent / "runtime.cid"
    )
    if result.returncode != 0:
        guard_error = result.stderr.decode("utf-8", "replace")[-1600:]
        server_error = _diagnostic_text(stderr_path)
        detail = "mcp-native-guard inspect failed: " + guard_error
        if server_error:
            detail += "\nserver stderr: " + server_error
        base.fail(detail)
    inventory = json.loads(result.stdout)
    base.validate_inventory(inventory)
    return inventory


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    observe = sub.add_parser("observe")
    for name in ("database", "server", "version", "package", "guard-binary", "evidence-root"):
        observe.add_argument("--" + name, required=True)
    observe.add_argument("--runtime-image", default=AUTO_RUNTIME_POLICY)
    observe.add_argument("--timeout", type=int, default=180)
    drift = sub.add_parser("compare")
    drift.add_argument("--database", required=True)
    drift.add_argument("--older-run-id", type=int, required=True)
    drift.add_argument("--newer-run-id", type=int, required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    db = sqlite3.connect(args.database)
    db.execute("PRAGMA foreign_keys=ON")
    db.executescript(base.SCHEMA)
    if args.command == "compare":
        print(base.canonical(base.compare(db, args.older_run_id, args.newer_run_id)))
        return 0

    row = base.resolve_package(db, args.server, args.version, args.package)
    arguments, environment = launch_declarations(db, int(row["package_id"]))
    metadata, _ = base.npm_metadata(row["package_identifier"], row["package_version"])
    artifact_bytes, _ = base.download_artifact(metadata)
    artifact_sha = base.sha256_bytes(artifact_bytes)
    guard = Path(args.guard_binary).resolve()
    if not guard.is_file():
        base.fail("guard binary does not exist")

    with tempfile.TemporaryDirectory(prefix="mcpo-runtime-") as temporary:
        root = Path(temporary)
        cache, work = root / "cache", root / "work"
        base.prepare_writable_directory(cache)
        base.prepare_writable_directory(work)
        artifact = root / "artifact.tgz"
        artifact.write_bytes(artifact_bytes)
        manifest = package_manifest(artifact)
        requirement = node_engine(manifest)
        bin_entry = base.package_bin(artifact, row["package_identifier"])
        guard_sha256 = base.sha256_file(guard)
        resolved_image = resolve_runtime_image(
            args.runtime_image, requirement, guard, root, min(args.timeout, 180)
        )
        profile = {
            "profile_version": 1,
            "registry": "npm",
            "transport": "stdio",
            "package": row["package_identifier"],
            "version": row["package_version"],
            "bin": bin_entry,
            "arguments": arguments,
            "declared_environment": environment,
            "install_scripts": False,
            "runtime_network": "none",
            "runtime_policy": args.runtime_image,
            "node_engine": requirement,
            "image": resolved_image,
            "guard_sha256": guard_sha256,
            "stderr_capture_bytes": MAX_SERVER_STDERR_BYTES,
        }
        profile_sha = base.sha256_bytes(base.canonical(profile).encode())
        base.populate_cache(resolved_image, cache, artifact, args.timeout)
        base.offline_install(resolved_image, cache, work, artifact, args.timeout)
        inventory = inspect_runtime(
            resolved_image,
            work,
            guard,
            row["package_identifier"],
            bin_entry,
            arguments,
            args.timeout,
        )
        run_id = base.persist(
            db,
            row,
            artifact_sha,
            profile_sha,
            resolved_image,
            "sha256:" + guard_sha256,
            inventory,
            Path(args.evidence_root),
        )

    print(base.canonical({
        "status": "completed",
        "runtime_observation_run_id": run_id,
        "artifact_sha256": artifact_sha,
        "launch_profile_sha256": profile_sha,
        "guard_sha256": guard_sha256,
        "runtime_policy": args.runtime_image,
        "runtime_image": resolved_image,
        "node_engine": requirement,
        "tool_count": len(inventory["tools"]),
    }))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, OSError, sqlite3.Error, json.JSONDecodeError, subprocess.TimeoutExpired) as exc:
        print(f"runtime discovery failed: {exc}", file=sys.stderr)
        raise SystemExit(2)
