#!/usr/bin/env python3
"""Offline Artifact Static Analysis v1 CLI tests."""

import hashlib
import http.server
import json
import pathlib
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import threading
import urllib.parse

# Reuse explorer fixture helpers.
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from test_explorer_cli import make_bundle, require, run  # noqa: E402


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha512_integrity(data: bytes) -> str:
    import base64

    return "sha512-" + base64.b64encode(hashlib.sha512(data).digest()).decode()


def make_tarball(root: pathlib.Path, files: dict[str, bytes]) -> bytes:
    import io
    import tarfile

    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode="w:gz") as archive:
        for name, data in files.items():
            info = tarfile.TarInfo(name)
            info.size = len(data)
            archive.addfile(info, io.BytesIO(data))
    return buffer.getvalue()


class NpmHandler(http.server.BaseHTTPRequestHandler):
    catalog = {}

    def log_message(self, format, *args):  # noqa: A003
        return

    def do_GET(self):  # noqa: N802
        path = urllib.parse.unquote(self.path)
        item = self.catalog.get(path)
        if item is None:
            self.send_response(404)
            self.end_headers()
            return
        body, content_type = item
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def serve_npm(catalog):
    NpmHandler.catalog = catalog
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), NpmHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    host, port = server.server_address
    return server, f"http://{host}:{port}"


def seed_database(binary, root: pathlib.Path):
    specs = [
        {
            "name": "io.example/demo-npm",
            "version": "1.0.1",
            "description": "npm demo",
            "packages": [
                {
                    "registryType": "npm",
                    "identifier": "demo-npm",
                    "version": "1.0.1",
                    "transport": {"type": "stdio"},
                },
                {
                    "registryType": "npm",
                    "identifier": "demo-npm",
                    "version": "1.0.1",
                    "transport": {"type": "streamable-http"},
                }
            ],
        },
        {
            "name": "io.example/ambiguous",
            "version": "1.0.0",
            "description": "two packages",
            "packages": [
                {
                    "registryType": "npm",
                    "identifier": "same-name",
                    "version": "2.0.0",
                    "transport": {"type": "streamable-http"},
                },
                {
                    "registryType": "npm",
                    "identifier": "same-name",
                    "version": "1.0.0",
                    "transport": {"type": "stdio"},
                },
            ],
        },
        {
            "name": "io.example/pypi-only",
            "version": "2.0.0",
            "description": "pypi",
            "packages": [
                {
                    "registryType": "pypi",
                    "identifier": "demo-pypi",
                    "version": "2.0.0",
                    "transport": {"type": "stdio"},
                }
            ],
        },
        {
            "name": "io.example/missing-version",
            "version": "3.0.0",
            "description": "no package version",
            "packages": [
                {
                    "registryType": "npm",
                    "identifier": "no-version-pkg",
                    "transport": {"type": "stdio"},
                }
            ],
        },
    ]
    # Ambiguous case needs two artifact versions with the same package identifier.
    bundle = root / "bundle"
    make_bundle(bundle, specs, "2026-07-27T00:00:00Z")
    database = root / "registry.sqlite"
    indexed = run(
        binary,
        "registry",
        "index",
        "--bundle",
        str(bundle),
        "--database",
        str(database),
    )
    require(indexed.returncode == 0, "index fixture database", indexed)
    # Force a second identical package row for ambiguity by duplicating in SQL after import
    # is not possible due UNIQUE(server_version_id, position). Instead insert another
    # server_versions variant with same identity fields for same-name package.
    connection = sqlite3.connect(database)
    row = connection.execute(
        "SELECT server_version_id FROM packages WHERE identifier='same-name' LIMIT 1"
    ).fetchone()
    require(row is not None, "ambiguous seed missing")
    server_version_id = row[0]
    # Duplicate package under next position already exists from two package entries.
    count = connection.execute(
        "SELECT COUNT(*) FROM packages WHERE identifier='same-name'"
    ).fetchone()[0]
    require(count == 2, f"expected two same-name packages, got {count}")
    connection.close()
    return database


def main(binary: str) -> None:
    with tempfile.TemporaryDirectory(prefix="mcpo-analyze-cli-") as temp:
        root = pathlib.Path(temp)
        database = seed_database(binary, root)
        rules = (
            pathlib.Path(__file__).resolve().parent.parent
            / "rules"
            / "artifact-static-analysis-v1.json"
        )

        # 1 exact resolution success path prep
        package_json = json.dumps(
            {
                "name": "demo-npm",
                "version": "1.0.1",
                "license": "MIT",
                "bin": {"demo-npm": "dist/server.js"},
                "scripts": {"prepare": "tsc"},
                "dependencies": {"zod": "3.0.0"},
                "engines": {"node": ">=20"},
            }
        ).encode()
        server_js = b"import fs from 'node:fs';\nimport url from 'node:url';\n"
        minified_js = (
            b"\n" * 6
            + b"const bundled='"
            + b"x" * 100_000
            + b"'; child_process.spawn('worker'); const tail='"
            + b"y" * 100_000
            + b"';\n"
        )
        tarball = make_tarball(
            root,
            {
                "package/package.json": package_json,
                "package/dist/server.js": server_js,
                "package/dist/minified.js": minified_js,
            },
        )
        integrity = sha512_integrity(tarball)
        metadata = {
            "name": "demo-npm",
            "version": "1.0.1",
            "dist": {
                "tarball": "PLACEHOLDER",
                "integrity": integrity,
            },
        }
        server, base = serve_npm({})
        metadata["dist"]["tarball"] = f"{base}/demo-npm/-/demo-npm-1.0.1.tgz"
        pypi_sdist = make_tarball(
            root,
            {
                "demo_pypi-2.0.0/PKG-INFO": (
                    b"Metadata-Version: 2.1\n"
                    b"Name: Demo_PyPI\n"
                    b"Version: 2.0.0\n"
                    b"License: MIT\n"
                    b"Requires-Dist: mcp>=1.0\n"
                ),
                "demo_pypi-2.0.0/demo_pypi/server.py": (
                    b"from pathlib import Path\n"
                    b"def main():\n"
                    b"    return Path.cwd()\n"
                ),
            },
        )
        pypi_digest = sha256(pypi_sdist)
        pypi_metadata = {
            "info": {"name": "Demo_PyPI", "version": "2.0.0"},
            "urls": [
                {
                    "filename": "demo_pypi-2.0.0-py3-none-any.whl",
                    "packagetype": "bdist_wheel",
                    "url": f"{base}/packages/demo_pypi-2.0.0.whl",
                    "size": 1,
                    "digests": {"sha256": "0" * 64},
                    "yanked": False,
                },
                {
                    "filename": "demo_pypi-2.0.0.tar.gz",
                    "packagetype": "sdist",
                    "url": f"{base}/packages/demo_pypi-2.0.0.tar.gz",
                    "size": len(pypi_sdist),
                    "digests": {"sha256": pypi_digest},
                    "yanked": False,
                },
            ],
        }
        NpmHandler.catalog = {
            "/demo-npm/1.0.1": (
                json.dumps(metadata).encode(),
                "application/json",
            ),
            "/demo-npm/-/demo-npm-1.0.1.tgz": (tarball, "application/octet-stream"),
            "/pypi/demo-pypi/2.0.0/json": (
                json.dumps(pypi_metadata).encode(),
                "application/json",
            ),
            "/packages/demo_pypi-2.0.0.tar.gz": (
                pypi_sdist,
                "application/octet-stream",
            ),
        }

        evidence = root / "evidence"
        first = run(
            binary,
            "analyze",
            "package",
            "--database",
            str(database),
            "--server",
            "io.example/demo-npm",
            "--version",
            "1.0.1",
            "--package",
            "demo-npm",
            "--evidence-root",
            str(evidence),
            "--rules",
            str(rules),
            "--npm-registry-url",
            base,
            "--allow-in-process-worker",
            "--format",
            "json",
        )
        require(first.returncode == 0, "analyze package failed", first)
        payload = json.loads(first.stdout)
        require(payload["status"] == "completed", "status")
        require(payload["analyzer_version"] == "1.1.0", "analyzer version")
        require(payload["integrity_verified"] is True, "integrity")
        require(payload["package_version"] == "1.0.1", "version")
        require(payload["reused_existing"] is False, "first run reused")
        require("prepare" in payload["lifecycle_scripts"], "prepare script")
        require(payload["native_code"] is False, "native")
        artifact_sha = payload["artifact_sha256"]
        require(artifact_sha == sha256(tarball), "artifact digest")
        run_id = payload["analysis_run_id"]

        connection = sqlite3.connect(database)
        require(
            connection.execute("SELECT schema_version FROM schema_info").fetchone()
            == (3,),
            "schema migrated to 3",
        )
        require(
            connection.execute(
                "SELECT p.position FROM analysis_runs ar "
                "JOIN packages p ON p.id=ar.package_id WHERE ar.id=?",
                (run_id,),
            ).fetchone()
            == (0,),
            "equivalent transport rows should select the lowest package position",
        )
        require(
            connection.execute(
                "SELECT COUNT(*) FROM analysis_findings WHERE analysis_run_id=?",
                (run_id,),
            ).fetchone()[0]
            > 0,
            "findings stored",
        )
        require(
            connection.execute(
                "SELECT COUNT(*) FROM analysis_evidence WHERE analysis_run_id=?",
                (run_id,),
            ).fetchone()[0]
            >= 10,
            "evidence rows stored",
        )
        require(
            connection.execute("PRAGMA foreign_key_check").fetchall() == [],
            "foreign keys",
        )
        # relative paths only
        for (relative,) in connection.execute(
            "SELECT relative_path FROM analysis_evidence"
        ):
            require(not relative.startswith("/"), f"absolute evidence path {relative}")
        finding_id = connection.execute(
            "SELECT id FROM analysis_findings "
            "WHERE analysis_run_id=? AND subject_path='package/dist/server.js' "
            "ORDER BY id LIMIT 1",
            (run_id,),
        ).fetchone()[0]
        large_finding_id = connection.execute(
            "SELECT id FROM analysis_findings "
            "WHERE analysis_run_id=? AND subject_path='package/dist/minified.js' "
            "AND rule_id='risk-api:spawn' ORDER BY id LIMIT 1",
            (run_id,),
        ).fetchone()[0]
        connection.close()

        evidence_dir = (
            evidence
            / "artifacts"
            / "sha256"
            / artifact_sha[:2]
            / artifact_sha
        )
        require((evidence_dir / "artifact.tgz").is_file(), "artifact evidence")
        require(
            (evidence_dir / "analysis-rules.json").is_file(),
            "analysis rules evidence",
        )
        require((evidence_dir / "findings.jsonl").is_file(), "findings evidence")

        source = run(
            binary,
            "evidence",
            "finding-source",
            "--database",
            str(database),
            "--evidence-root",
            str(evidence),
            "--finding-id",
            str(finding_id),
            "--format",
            "json",
        )
        require(source.returncode == 0, "finding source failed", source)
        source_payload = json.loads(source.stdout)
        require(
            source_payload["content"] == server_js.decode(),
            "finding source content",
            source,
        )
        require(
            source_payload["subject_path"] == "package/dist/server.js",
            "finding source path",
            source,
        )
        require(
            source_payload["displayed_byte_size"] == len(server_js)
            and source_payload["start_line"] == 1
            and source_payload["truncated_before"] is False
            and source_payload["truncated_after"] is False,
            "small finding source window metadata",
            source,
        )
        large_source = run(
            binary,
            "evidence",
            "finding-source",
            "--database",
            str(database),
            "--evidence-root",
            str(evidence),
            "--finding-id",
            str(large_finding_id),
            "--format",
            "json",
        )
        require(
            large_source.returncode == 0,
            "large finding source should return a bounded window",
            large_source,
        )
        large_payload = json.loads(large_source.stdout)
        require(
            large_payload["byte_size"] == len(minified_js)
            and large_payload["displayed_byte_size"] <= 128 * 1024
            and large_payload["start_line"] == 7
            and large_payload["truncated_before"] is True
            and large_payload["truncated_after"] is True
            and large_payload["starts_mid_line"] is True
            and large_payload["ends_mid_line"] is True,
            "large finding source window metadata",
            large_source,
        )
        require(
            "child_process.spawn" in large_payload["content"],
            "large finding source window is not centered on the finding symbol",
            large_source,
        )
        downloaded_source = run(
            binary,
            "evidence",
            "finding-source",
            "--database",
            str(database),
            "--evidence-root",
            str(evidence),
            "--finding-id",
            str(large_finding_id),
            "--format",
            "raw",
        )
        require(
            downloaded_source.returncode == 0
            and downloaded_source.stdout.encode() == minified_js,
            "raw finding source must return the complete verified file",
            downloaded_source,
        )
        missing_source = run(
            binary,
            "evidence",
            "finding-source",
            "--database",
            str(database),
            "--evidence-root",
            str(evidence),
            "--finding-id",
            "999999",
        )
        require(
            missing_source.returncode == 5,
            "missing finding source should fail",
            missing_source,
        )
        tampered_evidence = root / "tampered-evidence"
        shutil.copytree(evidence, tampered_evidence)
        tampered_artifact = (
            tampered_evidence
            / "artifacts"
            / "sha256"
            / artifact_sha[:2]
            / artifact_sha
            / "artifact.tgz"
        )
        tampered_artifact.write_bytes(
            tampered_artifact.read_bytes() + b"tampered"
        )
        tampered_source = run(
            binary,
            "evidence",
            "finding-source",
            "--database",
            str(database),
            "--evidence-root",
            str(tampered_evidence),
            "--finding-id",
            str(finding_id),
        )
        require(
            tampered_source.returncode == 7,
            "tampered finding source should fail closed",
            tampered_source,
        )

        schema_v2_database = root / "schema-v2.sqlite"
        shutil.copy2(database, schema_v2_database)
        migration = sqlite3.connect(schema_v2_database)
        migration.execute("DROP TABLE analysis_finding_reviews")
        migration.execute(
            "UPDATE schema_info SET schema_version=2 WHERE singleton=1"
        )
        migration.commit()
        migration.close()
        migrated_review = run(
            binary,
            "review",
            "finding",
            "--database",
            str(schema_v2_database),
            "--finding-id",
            str(finding_id),
            "--expected-disposition",
            "unreviewed",
            "--disposition",
            "reviewed-benign",
            "--reviewer",
            "migration-test",
            "--format",
            "json",
        )
        require(
            migrated_review.returncode == 0,
            "schema-v2 review migration failed",
            migrated_review,
        )
        migration = sqlite3.connect(schema_v2_database)
        require(
            migration.execute(
                "SELECT schema_version FROM schema_info"
            ).fetchone()
            == (3,),
            "review did not migrate schema to 3",
        )
        migration.close()

        reviewed = run(
            binary,
            "review",
            "finding",
            "--database",
            str(database),
            "--finding-id",
            str(finding_id),
            "--expected-disposition",
            "unreviewed",
            "--disposition",
            "expected",
            "--reviewer",
            "offline-test",
            "--format",
            "json",
        )
        require(reviewed.returncode == 0, "finding review failed", reviewed)
        review_payload = json.loads(reviewed.stdout)
        require(
            review_payload["previous_disposition"] == "unreviewed"
            and review_payload["disposition"] == "expected",
            "review transition",
            reviewed,
        )
        stale_review = run(
            binary,
            "review",
            "finding",
            "--database",
            str(database),
            "--finding-id",
            str(finding_id),
            "--expected-disposition",
            "unreviewed",
            "--disposition",
            "false-positive",
            "--reviewer",
            "offline-test",
        )
        require(stale_review.returncode == 7, "stale review should conflict", stale_review)
        connection = sqlite3.connect(database)
        require(
            connection.execute(
                "SELECT disposition FROM analysis_findings WHERE id=?",
                (finding_id,),
            ).fetchone()
            == ("expected",),
            "review updates current disposition",
        )
        require(
            connection.execute(
                "SELECT previous_disposition,disposition,reviewer "
                "FROM analysis_finding_reviews WHERE finding_id=?",
                (finding_id,),
            ).fetchone()
            == ("unreviewed", "expected", "offline-test"),
            "review audit row",
        )
        connection.close()

        # 25 deduplication
        second = run(
            binary,
            "analyze",
            "package",
            "--database",
            str(database),
            "--server",
            "io.example/demo-npm",
            "--version",
            "1.0.1",
            "--package",
            "demo-npm",
            "--evidence-root",
            str(evidence),
            "--npm-registry-url",
            base,
            "--allow-in-process-worker",
            "--format",
            "json",
        )
        require(second.returncode == 0, "dedupe analyze failed", second)
        reused = json.loads(second.stdout)
        require(reused["reused_existing"] is True, "expected reuse")
        require(reused["analysis_run_id"] == run_id, "same run id")

        # text output stable keys
        text = run(
            binary,
            "analyze",
            "package",
            "--database",
            str(database),
            "--server",
            "io.example/demo-npm",
            "--version",
            "1.0.1",
            "--package",
            "demo-npm",
            "--evidence-root",
            str(evidence),
            "--npm-registry-url",
            base,
            "--allow-in-process-worker",
            "--format",
            "text",
        )
        require(text.returncode == 0, "text output failed", text)
        for key in (
            "analysis_run_id=",
            "integrity_verified=true",
            "status=completed",
            "reused_existing=true",
        ):
            require(key in text.stdout, f"missing {key}", text)

        # When the fixed analyzer image is already available, exercise the real
        # UID 65532 Docker boundary under the portal launcher's restrictive
        # umask. The unit suite always checks the exact staging modes; this
        # integration check is skipped on hosts without a ready Docker daemon.
        docker_ready = subprocess.run(
            ["/usr/bin/docker", "image", "inspect", "debian:bookworm-slim"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            timeout=10,
            check=False,
        )
        if docker_ready.returncode == 0:
            restricted = subprocess.run(
                [
                    "/bin/sh",
                    "-c",
                    'umask 077; exec "$@"',
                    "sh",
                    binary,
                    "analyze",
                    "package",
                    "--database",
                    str(database),
                    "--server",
                    "io.example/demo-npm",
                    "--version",
                    "1.0.1",
                    "--package",
                    "demo-npm",
                    "--evidence-root",
                    str(evidence),
                    "--rules",
                    str(rules),
                    "--npm-registry-url",
                    base,
                    "--force",
                    "--format",
                    "json",
                ],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                timeout=180,
                check=False,
            )
            require(
                restricted.returncode == 0,
                "UID 65532 analyzer must read both mounts under umask 077",
                restricted,
            )
            require(
                json.loads(restricted.stdout)["reused_existing"] is False,
                "restricted analyzer regression must execute a fresh container",
                restricted,
            )

        # 2 ambiguous rejection
        ambiguous = run(
            binary,
            "analyze",
            "package",
            "--database",
            str(database),
            "--server",
            "io.example/ambiguous",
            "--version",
            "1.0.0",
            "--package",
            "same-name",
            "--evidence-root",
            str(evidence),
            "--allow-in-process-worker",
        )
        require(ambiguous.returncode == 5, "ambiguous should fail", ambiguous)
        require("ambiguous" in ambiguous.stderr.lower(), "ambiguous message", ambiguous)

        # Exact package-id selection bypasses an ambiguous identity triplet.
        connection = sqlite3.connect(database)
        selected_package_id = connection.execute(
            "SELECT id FROM packages WHERE identifier='same-name' AND version='1.0.0'"
        ).fetchone()[0]
        connection.close()
        exact_package_json = json.dumps(
            {"name": "same-name", "version": "1.0.0", "main": "index.js"}
        ).encode()
        exact_tarball = make_tarball(
            root,
            {
                "package/package.json": exact_package_json,
                "package/index.js": b"export const value = 1;\n",
            },
        )
        exact_metadata = {
            "name": "same-name",
            "version": "1.0.0",
            "dist": {
                "tarball": f"{base}/same-name/-/same-name-1.0.0.tgz",
                "integrity": sha512_integrity(exact_tarball),
            },
        }
        NpmHandler.catalog["/same-name/1.0.0"] = (
            json.dumps(exact_metadata).encode(),
            "application/json",
        )
        NpmHandler.catalog["/same-name/-/same-name-1.0.0.tgz"] = (
            exact_tarball,
            "application/octet-stream",
        )
        by_id = run(
            binary,
            "analyze",
            "package",
            "--database",
            str(database),
            "--package-id",
            str(selected_package_id),
            "--evidence-root",
            str(evidence),
            "--rules",
            str(rules),
            "--npm-registry-url",
            base,
            "--allow-in-process-worker",
            "--format",
            "json",
        )
        require(by_id.returncode == 0, "package-id analysis should complete", by_id)
        by_id_payload = json.loads(by_id.stdout)
        require(
            by_id_payload["package_identifier"] == "same-name"
            and by_id_payload["package_version"] == "1.0.0",
            "package-id selected the wrong record",
            by_id,
        )
        connection = sqlite3.connect(database)
        require(
            connection.execute(
                "SELECT package_id FROM analysis_runs WHERE id=?",
                (by_id_payload["analysis_run_id"],),
            ).fetchone()
            == (selected_package_id,),
            "package-id analysis run is attached to the wrong package record",
        )
        connection.close()
        conflicting_selector = run(
            binary,
            "analyze",
            "package",
            "--database",
            str(database),
            "--package-id",
            str(selected_package_id),
            "--server",
            "io.example/ambiguous",
            "--version",
            "1.0.0",
            "--package",
            "same-name",
        )
        require(
            conflicting_selector.returncode == 1,
            "package-id and identity triplet must be mutually exclusive",
            conflicting_selector,
        )

        # 3 exact PyPI sdist acquisition, integrity verification, and reuse
        pypi = run(
            binary,
            "analyze",
            "package",
            "--database",
            str(database),
            "--server",
            "io.example/pypi-only",
            "--version",
            "2.0.0",
            "--package",
            "demo-pypi",
            "--evidence-root",
            str(evidence),
            "--rules",
            str(rules),
            "--pypi-registry-url",
            f"{base}/pypi",
            "--allow-in-process-worker",
            "--format",
            "json",
        )
        require(pypi.returncode == 0, "PyPI analysis should complete", pypi)
        pypi_payload = json.loads(pypi.stdout)
        require(pypi_payload["registry_type"] == "pypi", "PyPI registry output")
        require(pypi_payload["integrity_verified"] is True, "PyPI integrity")
        require(
            pypi_payload["artifact_sha256"] == pypi_digest,
            "PyPI artifact digest",
            pypi,
        )
        pypi_run_id = pypi_payload["analysis_run_id"]
        pypi_evidence = (
            evidence
            / "artifacts"
            / "sha256"
            / pypi_digest[:2]
            / pypi_digest
        )
        require(
            (pypi_evidence / "registry-metadata.json").is_file(),
            "PyPI registry metadata evidence",
        )
        connection = sqlite3.connect(database)
        require(
            connection.execute(
                "SELECT registry_type,published_integrity "
                "FROM analysis_artifacts WHERE analysis_run_id=?",
                (pypi_run_id,),
            ).fetchone()
            == ("pypi", f"sha256:{pypi_digest}"),
            "PyPI artifact database row",
        )
        require(
            connection.execute(
                "SELECT COUNT(*) FROM analysis_dependencies WHERE analysis_run_id=? "
                "AND dependency_name='mcp'",
                (pypi_run_id,),
            ).fetchone()[0]
            == 1,
            "PyPI Requires-Dist dependency",
        )
        connection.close()

        pypi_reused = run(
            binary,
            "analyze",
            "package",
            "--database",
            str(database),
            "--server",
            "io.example/pypi-only",
            "--version",
            "2.0.0",
            "--package",
            "demo-pypi",
            "--evidence-root",
            str(evidence),
            "--rules",
            str(rules),
            "--pypi-registry-url",
            f"{base}/pypi",
            "--allow-in-process-worker",
            "--format",
            "json",
        )
        require(pypi_reused.returncode == 0, "PyPI reuse should complete", pypi_reused)
        require(
            json.loads(pypi_reused.stdout)["reused_existing"] is True,
            "PyPI completed run should be reused",
            pypi_reused,
        )

        # 4 missing exact package version
        missing = run(
            binary,
            "analyze",
            "package",
            "--database",
            str(database),
            "--server",
            "io.example/missing-version",
            "--version",
            "3.0.0",
            "--package",
            "no-version-pkg",
            "--evidence-root",
            str(evidence),
            "--allow-in-process-worker",
        )
        require(missing.returncode == 5, "missing version should fail", missing)
        require("exact package version" in missing.stderr.lower(), "missing message", missing)

        # failed-run recording via integrity mismatch
        bad_meta = dict(metadata)
        bad_meta["dist"] = dict(metadata["dist"])
        bad_meta["dist"]["integrity"] = (
            "sha512-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=="
        )
        NpmHandler.catalog["/demo-npm/1.0.1"] = (
            json.dumps(bad_meta).encode(),
            "application/json",
        )
        failed = run(
            binary,
            "analyze",
            "package",
            "--database",
            str(database),
            "--server",
            "io.example/demo-npm",
            "--version",
            "1.0.1",
            "--package",
            "demo-npm",
            "--evidence-root",
            str(evidence),
            "--npm-registry-url",
            base,
            "--allow-in-process-worker",
            "--force",
        )
        require(failed.returncode == 7, "integrity failure exit", failed)
        connection = sqlite3.connect(database)
        require(
            connection.execute(
                "SELECT COUNT(*) FROM analysis_runs WHERE status='failed' AND error_stage='integrity'"
            ).fetchone()[0]
            >= 1,
            "failed run recorded",
        )
        connection.close()

        # worker JSON stability on fixture
        worker = run(
            binary,
            "analyze-worker",
            "--registry",
            "npm",
            "--tarball",
            str(evidence_dir / "artifact.tgz"),
        )
        require(worker.returncode == 0, "worker failed", worker)
        worker_json = json.loads(worker.stdout)
        require(worker_json["package_name"] == "demo-npm", "worker name")
        require(worker_json["status"] == "ok", "worker status")

        server.shutdown()
        print("analyze cli tests passed")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("usage: test_analyze_cli.py PATH_TO_mcp-observatory", file=sys.stderr)
        sys.exit(2)
    main(sys.argv[1])
