#!/usr/bin/env python3
"""Compact one completed static-analysis result into Storage v2 evidence.

The legacy analyzer is intentionally left unchanged for the MVP. It may write its
normal per-artifact evidence directory first. This tool then creates one
content-addressed, deterministic gzip/tar evidence bundle, stores the ruleset
once by digest, optionally retains the exact package artifact, and records the
bundle in ``analysis_v2_evidence_manifests``.

Legacy files are never removed unless ``--prune-legacy`` is explicitly passed.
Pruning is refused when more than one completed run references the same artifact
because the legacy directory is keyed only by artifact digest.

Only the Python standard library is used.
"""

from __future__ import annotations

import argparse
from contextlib import contextmanager
import fcntl
import gzip
import hashlib
import io
import os
from pathlib import Path
import shutil
import sqlite3
import tarfile
from typing import Iterator

BUNDLE_MEMBERS = (
    "findings.jsonl",
    "package-manifest.json",
    "dependencies.json",
    "archive-inventory.json",
    "registry-metadata.json",
    "analysis-summary.json",
    "analyzer.log",
    "files.jsonl",
)


@contextmanager
def writer_lock(database: Path) -> Iterator[None]:
    path = Path(str(database) + ".writer.lock")
    descriptor = os.open(
        path, os.O_RDWR | os.O_CREAT | os.O_CLOEXEC | os.O_NOFOLLOW, 0o600
    )
    try:
        while True:
            try:
                fcntl.flock(descriptor, fcntl.LOCK_EX)
                break
            except InterruptedError:
                continue
        yield
    finally:
        os.close(descriptor)


def connect(database: Path) -> sqlite3.Connection:
    db = sqlite3.connect(database, timeout=30)
    db.row_factory = sqlite3.Row
    db.execute("PRAGMA foreign_keys=ON")
    db.execute("PRAGMA busy_timeout=30000")
    return db


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def atomic_copy(source: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists():
        return
    temporary = destination.with_name(destination.name + f".tmp-{os.getpid()}")
    try:
        with source.open("rb") as src, temporary.open("xb") as dst:
            shutil.copyfileobj(src, dst, length=1024 * 1024)
            dst.flush()
            os.fsync(dst.fileno())
        os.replace(temporary, destination)
    finally:
        temporary.unlink(missing_ok=True)


def bundle_bytes(directory: Path) -> tuple[bytes, list[str]]:
    raw = io.BytesIO()
    included: list[str] = []
    with gzip.GzipFile(fileobj=raw, mode="wb", mtime=0) as compressed:
        with tarfile.open(fileobj=compressed, mode="w", format=tarfile.PAX_FORMAT) as archive:
            for name in BUNDLE_MEMBERS:
                path = directory / name
                if not path.is_file():
                    continue
                data = path.read_bytes()
                info = tarfile.TarInfo(name=name)
                info.size = len(data)
                info.mode = 0o600
                info.mtime = 0
                info.uid = 0
                info.gid = 0
                info.uname = ""
                info.gname = ""
                archive.addfile(info, io.BytesIO(data))
                included.append(name)
    return raw.getvalue(), included


def write_atomic_bytes(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + f".tmp-{os.getpid()}")
    try:
        with temporary.open("xb") as output:
            output.write(data)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def require_v2(db: sqlite3.Connection) -> None:
    tables = {
        str(row[0])
        for row in db.execute("SELECT name FROM sqlite_schema WHERE type='table'")
    }
    required = {"analysis_runs", "analysis_v2_evidence_manifests"}
    missing = sorted(required - tables)
    if missing:
        raise RuntimeError(
            "Storage v2 foundation is not installed; missing: " + ", ".join(missing)
        )


def process(
    database: Path,
    evidence_root: Path,
    output_root: Path,
    run_id: int,
    artifact_retention: str,
    prune_legacy: bool,
) -> dict[str, object]:
    with writer_lock(database):
        db = connect(database)
        try:
            require_v2(db)
            row = db.execute(
                """SELECT id,status,artifact_sha256,ruleset_version
                   FROM analysis_runs WHERE id=?""",
                (run_id,),
            ).fetchone()
            if row is None:
                raise ValueError(f"analysis run {run_id} does not exist")
            if str(row["status"]) != "completed":
                raise ValueError(f"analysis run {run_id} is not completed")
            artifact = str(row["artifact_sha256"] or "")
            if len(artifact) != 64 or any(c not in "0123456789abcdef" for c in artifact):
                raise ValueError(f"analysis run {run_id} has no canonical artifact digest")

            existing = db.execute(
                "SELECT * FROM analysis_v2_evidence_manifests WHERE analysis_run_id=?",
                (run_id,),
            ).fetchone()
            if existing is not None and str(existing["storage_kind"]) == "bundle":
                return {
                    "analysis_run_id": run_id,
                    "artifact_sha256": artifact,
                    "status": "already-bundled",
                    "locator": str(existing["locator"]),
                    "bundle_sha256": str(existing["bundle_sha256"] or ""),
                }

            legacy = evidence_root / "artifacts" / "sha256" / artifact[:2] / artifact
            if not legacy.is_dir():
                raise FileNotFoundError(f"legacy evidence directory is missing: {legacy}")

            rules_source = legacy / "analysis-rules.json"
            rules_sha256 = ""
            if rules_source.is_file():
                rules_sha256 = sha256_file(rules_source)
                rules_destination = (
                    output_root / "rulesets" / "sha256" / rules_sha256[:2] /
                    f"{rules_sha256}.json"
                )
                atomic_copy(rules_source, rules_destination)

            bundle, included = bundle_bytes(legacy)
            if not included:
                raise RuntimeError(f"legacy evidence directory contains no bundle members: {legacy}")
            bundle_sha256 = hashlib.sha256(bundle).hexdigest()
            bundle_path = (
                output_root / "bundles" / "sha256" / artifact[:2] / artifact /
                f"run-{run_id}-{bundle_sha256}.tar.gz"
            )
            if not bundle_path.exists():
                write_atomic_bytes(bundle_path, bundle)
            elif sha256_file(bundle_path) != bundle_sha256:
                raise RuntimeError(f"existing bundle digest mismatch: {bundle_path}")

            inventory = legacy / "files.jsonl"
            inventory_sha256 = sha256_file(inventory) if inventory.is_file() else None

            retained_artifact = 0
            artifact_source = legacy / "artifact.tgz"
            if artifact_retention == "all" and artifact_source.is_file():
                retained_path = (
                    output_root / "retained-artifacts" / "sha256" / artifact[:2] /
                    f"{artifact}.tgz"
                )
                atomic_copy(artifact_source, retained_path)
                if sha256_file(retained_path) != artifact:
                    raise RuntimeError("retained artifact digest does not match analysis artifact")
                retained_artifact = 1

            locator = bundle_path.relative_to(output_root).as_posix()
            with db:
                db.execute(
                    """INSERT INTO analysis_v2_evidence_manifests(
                         analysis_run_id,storage_kind,locator,bundle_sha256,
                         inventory_sha256,retained_artifact)
                       VALUES(?,?,?,?,?,?)
                       ON CONFLICT(analysis_run_id) DO UPDATE SET
                         storage_kind=excluded.storage_kind,
                         locator=excluded.locator,
                         bundle_sha256=excluded.bundle_sha256,
                         inventory_sha256=excluded.inventory_sha256,
                         retained_artifact=excluded.retained_artifact,
                         updated_at=CURRENT_TIMESTAMP""",
                    (
                        run_id,
                        "bundle",
                        locator,
                        bundle_sha256,
                        inventory_sha256,
                        retained_artifact,
                    ),
                )

            pruned = False
            if prune_legacy:
                references = int(
                    db.execute(
                        """SELECT COUNT(*) FROM analysis_runs
                           WHERE status='completed' AND artifact_sha256=?""",
                        (artifact,),
                    ).fetchone()[0]
                )
                if references != 1:
                    raise RuntimeError(
                        "refusing to prune legacy evidence shared by "
                        f"{references} completed analysis runs"
                    )
                shutil.rmtree(legacy)
                pruned = True

            return {
                "analysis_run_id": run_id,
                "artifact_sha256": artifact,
                "bundle_sha256": bundle_sha256,
                "bundle_bytes": len(bundle),
                "bundle_members": included,
                "rules_sha256": rules_sha256,
                "retained_artifact": bool(retained_artifact),
                "legacy_pruned": pruned,
                "locator": locator,
                "status": "bundled",
            }
        finally:
            db.close()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--database", required=True, type=Path)
    parser.add_argument("--evidence-root", required=True, type=Path)
    parser.add_argument("--output-root", required=True, type=Path)
    parser.add_argument("--analysis-run-id", required=True, type=int)
    parser.add_argument(
        "--artifact-retention", choices=("none", "all"), default="none"
    )
    parser.add_argument("--prune-legacy", action="store_true")
    args = parser.parse_args()
    if args.analysis_run_id <= 0:
        parser.error("--analysis-run-id must be positive")
    return args


def main() -> int:
    import json

    args = parse_args()
    result = process(
        args.database.resolve(),
        args.evidence_root.resolve(),
        args.output_root.resolve(),
        args.analysis_run_id,
        args.artifact_retention,
        args.prune_legacy,
    )
    print(json.dumps(result, sort_keys=True, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, sqlite3.Error, RuntimeError, ValueError) as exc:
        print(f"storage v2 evidence failed: {exc}", file=__import__("sys").stderr)
        raise SystemExit(2)
