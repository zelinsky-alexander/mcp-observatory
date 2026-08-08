#!/usr/bin/env python3
"""Build and operate a side-by-side Storage v2 MVP.

The live v1 catalog is never modified by this tool. It can:

* clone a source catalog into an authoritative history/control database;
* install/backfill the additive Storage v2 summary schema there;
* derive a compact hot catalog by removing bulky v1 detail rows and VACUUMing;
* publish newly completed analysis/runtime summary rows from history to hot;
* create deterministic content-addressed evidence bundles without artifact.tgz
  or duplicated analysis-rules.json and register their locators;
* verify summary equivalence between history and hot catalogs.

Only Python's standard library and SQLite are used.
"""

from __future__ import annotations

import argparse
from contextlib import contextmanager
import fcntl
import gzip
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import sqlite3
import tarfile
import tempfile
from typing import Any, Iterator

MAX_BUNDLE_FILE_BYTES = 16 * 1024 * 1024
DEFAULT_EXCLUDED = {"artifact.tgz", "analysis-rules.json"}


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


@contextmanager
def writer_lock(database: Path) -> Iterator[None]:
    path = Path(str(database) + ".writer.lock")
    descriptor = os.open(path, os.O_RDWR | os.O_CREAT | os.O_CLOEXEC | os.O_NOFOLLOW, 0o600)
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


def connect(path: Path, *, readonly: bool = False) -> sqlite3.Connection:
    if readonly:
        uri = f"file:{path.resolve().as_posix()}?mode=ro"
        db = sqlite3.connect(uri, uri=True, timeout=30)
        db.execute("PRAGMA query_only=ON")
    else:
        db = sqlite3.connect(path, timeout=30)
    db.row_factory = sqlite3.Row
    db.execute("PRAGMA foreign_keys=ON")
    db.execute("PRAGMA busy_timeout=30000")
    return db


def load_foundation():
    path = Path(__file__).with_name("storage_v2_foundation.py")
    spec = importlib.util.spec_from_file_location("storage_v2_foundation", path)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load storage_v2_foundation.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def clone_database(source: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists():
        raise FileExistsError(f"destination already exists: {destination}")
    source_db = connect(source, readonly=True)
    try:
        destination_db = sqlite3.connect(destination)
        try:
            source_db.backup(destination_db, pages=4096)
            destination_db.execute("PRAGMA foreign_keys=ON")
            integrity = destination_db.execute("PRAGMA integrity_check").fetchone()[0]
            if integrity != "ok":
                raise RuntimeError(f"cloned database failed integrity_check: {integrity}")
        finally:
            destination_db.close()
    finally:
        source_db.close()


def install_and_backfill(database: Path, batch_size: int) -> int:
    foundation = load_foundation()
    processed_total = 0
    with writer_lock(database):
        db = connect(database)
        try:
            with db:
                foundation.install(db)
            while True:
                with db:
                    processed = foundation.backfill(db, batch_size)
                processed_total += processed
                if processed < batch_size:
                    break
            with db:
                foundation.refresh_coverage(db)
        finally:
            db.close()
    return processed_total


def _table_exists(db: sqlite3.Connection, name: str) -> bool:
    return db.execute(
        "SELECT 1 FROM sqlite_schema WHERE type='table' AND name=?", (name,)
    ).fetchone() is not None


def compact_hot_catalog(history: Path, hot: Path) -> dict[str, int]:
    clone_database(history, hot)
    purge_tables = (
        "analysis_finding_reviews",
        "analysis_findings",
        "analysis_files",
        "analysis_dependencies",
        "analysis_evidence",
        "analysis_artifacts",
        "static_analysis_schedule_state",
    )
    before = hot.stat().st_size
    with writer_lock(hot):
        db = connect(hot)
        try:
            db.execute("PRAGMA foreign_keys=OFF")
            for table in purge_tables:
                if _table_exists(db, table):
                    db.execute(f"DELETE FROM {table}")
            db.commit()
            db.execute("PRAGMA optimize")
            db.execute("VACUUM")
            check = db.execute("PRAGMA integrity_check").fetchone()[0]
            if check != "ok":
                raise RuntimeError(f"hot catalog failed integrity_check: {check}")
        finally:
            db.close()
    return {"before_bytes": before, "after_bytes": hot.stat().st_size}


def _columns(db: sqlite3.Connection, table: str) -> list[str]:
    return [str(row["name"]) for row in db.execute(f"PRAGMA table_info({table})")]


def _copy_rows(source: sqlite3.Connection, target: sqlite3.Connection, table: str) -> int:
    if not _table_exists(source, table) or not _table_exists(target, table):
        return 0
    columns = _columns(source, table)
    if columns != _columns(target, table):
        raise RuntimeError(f"schema mismatch while publishing {table}")
    names = ",".join(columns)
    placeholders = ",".join("?" for _ in columns)
    rows = source.execute(f"SELECT {names} FROM {table}").fetchall()
    if not rows:
        return 0
    target.executemany(
        f"INSERT OR REPLACE INTO {table}({names}) VALUES({placeholders})",
        [tuple(row[name] for name in columns) for row in rows],
    )
    return len(rows)


def publish_summaries(history: Path, hot: Path) -> dict[str, int]:
    """Publish bounded read-model state without copying v1 detail tables."""
    tables = (
        "storage_v2_info",
        "analysis_v2_rule_definitions",
        "analysis_v2_run_summaries",
        "analysis_v2_rule_summaries",
        "analysis_v2_evidence_manifests",
        "analysis_v2_coverage_summary",
        "analysis_runs",
        "runtime_observation_runs",
        "runtime_observation_tools",
    )
    with writer_lock(hot):
        source = connect(history, readonly=True)
        target = connect(hot)
        try:
            target.execute("PRAGMA foreign_keys=OFF")
            counts: dict[str, int] = {}
            with target:
                for table in tables:
                    counts[table] = _copy_rows(source, target, table)
            return counts
        finally:
            target.close()
            source.close()


def verify(history: Path, hot: Path) -> dict[str, Any]:
    h = connect(history, readonly=True)
    p = connect(hot, readonly=True)
    try:
        metrics: dict[str, Any] = {}
        for table in (
            "snapshots", "server_versions", "packages", "analysis_runs",
            "analysis_v2_run_summaries", "analysis_v2_rule_definitions",
            "analysis_v2_rule_summaries", "analysis_v2_evidence_manifests",
            "runtime_observation_runs",
        ):
            if _table_exists(h, table) and _table_exists(p, table):
                left = int(h.execute(f"SELECT COUNT(*) FROM {table}").fetchone()[0])
                right = int(p.execute(f"SELECT COUNT(*) FROM {table}").fetchone()[0])
                metrics[table] = {"history": left, "hot": right, "equal": left == right}
        if _table_exists(h, "analysis_v2_coverage_summary"):
            left = [tuple(row) for row in h.execute(
                "SELECT * FROM analysis_v2_coverage_summary ORDER BY profile_key"
            )]
            right = [tuple(row) for row in p.execute(
                "SELECT * FROM analysis_v2_coverage_summary ORDER BY profile_key"
            )]
            metrics["coverage_equal"] = left == right
        metrics["hot_detail_rows"] = {}
        for table in (
            "analysis_files", "analysis_findings", "analysis_dependencies",
            "analysis_evidence", "analysis_artifacts",
        ):
            if _table_exists(p, table):
                metrics["hot_detail_rows"][table] = int(
                    p.execute(f"SELECT COUNT(*) FROM {table}").fetchone()[0]
                )
        metrics["history_bytes"] = history.stat().st_size
        metrics["hot_bytes"] = hot.stat().st_size
        metrics["size_ratio"] = round(metrics["hot_bytes"] / max(1, metrics["history_bytes"]), 4)
        return metrics
    finally:
        p.close()
        h.close()


def _safe_members(directory: Path) -> list[Path]:
    members: list[Path] = []
    for path in sorted(directory.iterdir(), key=lambda item: item.name):
        if path.name in DEFAULT_EXCLUDED or not path.is_file() or path.is_symlink():
            continue
        if path.stat().st_size > MAX_BUNDLE_FILE_BYTES:
            raise RuntimeError(f"evidence member exceeds limit: {path}")
        members.append(path)
    return members


def deterministic_bundle(source_dir: Path, destination: Path) -> dict[str, Any]:
    members = _safe_members(source_dir)
    if not members:
        raise RuntimeError(f"no bundleable evidence files in {source_dir}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    tmp = destination.with_suffix(destination.suffix + ".tmp")
    with tmp.open("wb") as raw:
        with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0) as zipped:
            with tarfile.open(fileobj=zipped, mode="w") as archive:
                for path in members:
                    info = archive.gettarinfo(str(path), arcname=path.name)
                    info.uid = info.gid = 0
                    info.uname = info.gname = ""
                    info.mtime = 0
                    with path.open("rb") as stream:
                        archive.addfile(info, stream)
    os.replace(tmp, destination)
    return {
        "source": str(source_dir),
        "bundle": str(destination),
        "bundle_sha256": sha256_file(destination),
        "bundle_bytes": destination.stat().st_size,
        "member_count": len(members),
        "excluded": sorted(DEFAULT_EXCLUDED),
    }


def content_address_bundle(source_dir: Path, destination_root: Path) -> dict[str, Any]:
    destination_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="mcpo-v2-bundle-", dir=destination_root) as temporary:
        candidate = Path(temporary) / "evidence.tar.gz"
        result = deterministic_bundle(source_dir, candidate)
        digest = result["bundle_sha256"]
        final = destination_root / "sha256" / digest[:2] / f"{digest}.tar.gz"
        final.parent.mkdir(parents=True, exist_ok=True)
        if final.exists():
            if sha256_file(final) != digest:
                raise RuntimeError(f"existing content-addressed bundle has wrong digest: {final}")
        else:
            os.replace(candidate, final)
        result["bundle"] = str(final)
        result["bundle_bytes"] = final.stat().st_size
        return result


def _register_bundle_for_artifact(
    database: Path, artifact_sha256: str, result: dict[str, Any], destination_root: Path,
    inventory_sha256: str | None,
) -> int:
    with writer_lock(database):
        db = connect(database)
        try:
            if not _table_exists(db, "analysis_v2_evidence_manifests"):
                raise RuntimeError("Storage v2 foundation is not installed")
            runs = db.execute(
                """SELECT id FROM analysis_runs
                   WHERE status='completed' AND artifact_sha256=?
                   ORDER BY id""",
                (artifact_sha256,),
            ).fetchall()
            locator = Path(result["bundle"]).resolve().relative_to(destination_root.resolve()).as_posix()
            with db:
                for row in runs:
                    db.execute(
                        """INSERT INTO analysis_v2_evidence_manifests(
                             analysis_run_id,storage_kind,locator,bundle_sha256,
                             inventory_sha256,retained_artifact)
                           VALUES(?,'bundle',?,?,?,0)
                           ON CONFLICT(analysis_run_id) DO UPDATE SET
                             storage_kind='bundle',locator=excluded.locator,
                             bundle_sha256=excluded.bundle_sha256,
                             inventory_sha256=excluded.inventory_sha256,
                             retained_artifact=0,updated_at=CURRENT_TIMESTAMP""",
                        (int(row["id"]), locator, result["bundle_sha256"], inventory_sha256),
                    )
            return len(runs)
        finally:
            db.close()


def bundle_evidence(
    source_root: Path,
    destination_root: Path,
    limit: int,
    history_database: Path | None = None,
) -> list[dict[str, Any]]:
    """Bundle available per-artifact evidence.

    A fresh Storage v2 state may legitimately contain no artifact evidence
    directory yet. This happens when no newly processed artifact has emitted
    evidence, or when a bounded scheduler run performs no successful analysis.

    Absence of the evidence tree therefore means "nothing to bundle", not an
    invalid Storage v2 state.
    """
    sha_root = source_root / "artifacts" / "sha256"
    if not sha_root.is_dir():
        return []

    results: list[dict[str, Any]] = []

    for prefix in sorted(sha_root.iterdir()):
        if not prefix.is_dir():
            continue

        for artifact in sorted(prefix.iterdir()):
            if not artifact.is_dir() or len(artifact.name) != 64:
                continue

            result = content_address_bundle(artifact, destination_root)

            inventory = artifact / "archive-inventory.json"
            inventory_sha = (
                sha256_file(inventory)
                if inventory.is_file()
                else None
            )

            result["artifact_sha256"] = artifact.name
            result["registered_runs"] = 0

            if history_database is not None:
                result["registered_runs"] = _register_bundle_for_artifact(
                    history_database,
                    artifact.name,
                    result,
                    destination_root,
                    inventory_sha,
                )

            results.append(result)

            if limit and len(results) >= limit:
                return results

    return results


def prepare(args: argparse.Namespace) -> dict[str, Any]:
    source = args.source.resolve()
    history = args.history.resolve()
    hot = args.hot.resolve()
    clone_database(source, history)
    backfilled = install_and_backfill(history, args.batch_size)
    sizes = compact_hot_catalog(history, hot)
    published = publish_summaries(history, hot)
    return {
        "history": str(history), "hot": str(hot),
        "backfilled_runs": backfilled, "hot_compaction": sizes,
        "published": published, "verification": verify(history, hot),
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("prepare", help="clone live catalog and build history + compact hot catalogs")
    p.add_argument("--source", required=True, type=Path)
    p.add_argument("--history", required=True, type=Path)
    p.add_argument("--hot", required=True, type=Path)
    p.add_argument("--batch-size", type=int, default=250)

    p = sub.add_parser("publish", help="publish v2 summaries from history to hot catalog")
    p.add_argument("--history", required=True, type=Path)
    p.add_argument("--hot", required=True, type=Path)

    p = sub.add_parser("verify", help="compare hot read model with history/control database")
    p.add_argument("--history", required=True, type=Path)
    p.add_argument("--hot", required=True, type=Path)

    p = sub.add_parser("bundle-evidence", help="build deterministic compact evidence bundles")
    p.add_argument("--source-root", required=True, type=Path)
    p.add_argument("--destination-root", required=True, type=Path)
    p.add_argument("--history-database", type=Path)
    p.add_argument("--limit", type=int, default=100)

    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if getattr(args, "batch_size", 1) < 1 or getattr(args, "batch_size", 1) > 10_000:
        raise ValueError("batch size must be between 1 and 10000")
    if getattr(args, "limit", 0) < 0:
        raise ValueError("limit must be non-negative")
    if args.command == "prepare":
        result = prepare(args)
    elif args.command == "publish":
        result = publish_summaries(args.history.resolve(), args.hot.resolve())
    elif args.command == "verify":
        result = verify(args.history.resolve(), args.hot.resolve())
    else:
        result = bundle_evidence(
            args.source_root.resolve(), args.destination_root.resolve(), args.limit,
            args.history_database.resolve() if args.history_database else None,
        )
    print(json.dumps(result, sort_keys=True, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
