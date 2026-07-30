#!/usr/bin/env python3
"""Small, offline Local Registry Explorer v0.1 CLI tests."""

import hashlib
import json
import os
import pathlib
import shutil
import sqlite3
import subprocess
import sys
import tempfile


def encoded(value):
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"), sort_keys=True)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def canonical_record(spec, observed_at):
    server = {
        "name": spec["name"],
        "version": spec["version"],
        "description": spec["description"],
    }
    for key in ("repository", "packages", "remotes"):
        if key in spec:
            server[key] = spec[key]
    official = {
        "publishedAt": spec.get("published_at", "2026-01-01T00:00:00Z"),
        "updatedAt": spec.get("updated_at", "2026-01-02T00:00:00Z"),
        "status": spec.get("status", "active"),
    }
    original = {
        "_meta": {"io.modelcontextprotocol.registry/official": official},
        "server": server,
    }
    logical = {
        "description": spec["description"],
        "original": original,
        "record_version": 1,
        "registry": "official-mcp",
        "server_identifier": spec["name"],
        "server_version": spec["version"],
    }
    for key in ("repository", "packages", "remotes"):
        if key in spec:
            logical[key] = spec[key]
    digest = sha256(encoded(logical).encode())
    complete = dict(logical)
    complete["canonical_sha256"] = digest
    complete["observed_at"] = observed_at
    return complete


def make_bundle(path, specs, completed_at, observed_at="2026-01-01T00:00:00Z"):
    (path / "raw").mkdir(parents=True)
    (path / "canonical").mkdir()
    (path / "diagnostics").mkdir()
    records = [canonical_record(spec, observed_at) for spec in specs]
    records.sort(key=lambda item: (item["server_identifier"], item["server_version"]))
    canonical = "".join(encoded(record) + "\n" for record in records).encode()
    (path / "canonical" / "servers.jsonl").write_bytes(canonical)

    raw = encoded({"metadata": {"nextCursor": None}, "servers": []}).encode()
    (path / "raw" / "page-000001.json").write_bytes(raw)
    page = {
        "effective_response_url": "https://registry.example/v0.1/servers",
        "path": "raw/page-000001.json",
        "request_url": "https://registry.example/v0.1/servers",
        "response_bytes": len(raw),
        "retrieved_at": "2026-01-01T00:00:00Z",
        "sha256": sha256(raw),
    }
    pages = (encoded(page) + "\n").encode()
    (path / "raw" / "pages.jsonl").write_bytes(pages)
    (path / "diagnostics" / "errors.jsonl").write_bytes(b"")

    artifacts = []
    for relative in (
        "raw/page-000001.json",
        "raw/pages.jsonl",
        "canonical/servers.jsonl",
        "diagnostics/errors.jsonl",
    ):
        data = (path / relative).read_bytes()
        artifacts.append({"path": relative, "sha256": sha256(data), "size": len(data)})
    manifest = {
        "artifacts": artifacts,
        "bundle_version": 1,
        "collector": {
            "git_commit": "fixture",
            "name": "mcp-observatory",
            "version": "test",
        },
        "completed_at": completed_at,
        "counts": {
            "pages": 1,
            "records_received": len(records),
            "unique_server_versions": len(records),
        },
        "registry": "official-mcp",
        "registry_base_url": "https://registry.example",
        "snapshot_sha256": sha256(canonical),
        "started_at": "2026-01-01T00:00:00Z",
        "status": "complete",
    }
    (path / "manifest.json").write_text(encoded(manifest) + "\n")
    (path / "_SUCCESS").write_bytes(b"")
    return manifest["snapshot_sha256"]


def refresh_canonical_metadata(path, count=None):
    canonical = (path / "canonical" / "servers.jsonl").read_bytes()
    manifest_path = path / "manifest.json"
    manifest = json.loads(manifest_path.read_text())
    for artifact in manifest["artifacts"]:
        if artifact["path"] == "canonical/servers.jsonl":
            artifact["sha256"] = sha256(canonical)
            artifact["size"] = len(canonical)
    manifest["snapshot_sha256"] = sha256(canonical)
    if count is not None:
        manifest["counts"]["records_received"] = count
        manifest["counts"]["unique_server_versions"] = count
    manifest_path.write_text(encoded(manifest) + "\n")


def run(binary, *arguments):
    return subprocess.run(
        [binary, *map(str, arguments)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=30,
        check=False,
        env={"PATH": os.environ.get("PATH", "")},
    )


def require(condition, message, result=None):
    if condition:
        return
    detail = ""
    if result is not None:
        detail = f"\nstdout: {result.stdout}\nstderr: {result.stderr}"
    raise AssertionError(message + detail)


def main():
    binary = os.path.abspath(sys.argv[1])
    root = pathlib.Path(tempfile.mkdtemp(prefix="mcpo-explorer-test-"))
    specs = [
        {
            "name": "io.example/alpha",
            "version": "1.0.0",
            "description": "GitHub alpha database server",
            "repository": {
                "source": "github",
                "url": "https://github.com/example/alpha",
            },
            "packages": [
                {
                    "registryType": "npm",
                    "identifier": "@example/alpha",
                    "version": "1.0.0",
                    "transport": {"type": "stdio"},
                    "packageArguments": [
                        {"type": "positional", "value": "serve"},
                        {"type": "named", "value": "--safe"},
                        {"type": "positional", "valueHint": "workspace"},
                    ],
                    "environmentVariables": [
                        {
                            "name": "ALPHA_TOKEN",
                            "isRequired": True,
                            "description": "API token metadata",
                        }
                    ],
                }
            ],
        },
        {
            "name": "io.example/alpha",
            "version": "2.0.0",
            "description": "Hybrid alpha server",
            "status": "deprecated",
            "packages": [
                {
                    "registryType": "pypi",
                    "identifier": "alpha-mcp",
                    "transport": {"type": "stdio"},
                }
            ],
            "remotes": [
                {"url": "https://alpha.example:8443/mcp", "type": "streamable-http"},
                {"url": "https://alpha.example/events", "type": "sse"},
            ],
        },
        {
            "name": "io.example/beta",
            "version": "9",
            "description": "Remote beta search service",
            "remotes": [{"url": "https://remote.example/mcp", "type": "sse"}],
        },
    ]
    try:
        bundle = root / "bundle"
        digest = make_bundle(
            bundle, specs, "2026-01-03T00:00:00Z"
        )
        database = root / "catalog.sqlite"
        before = {
            path.relative_to(bundle): sha256(path.read_bytes())
            for path in bundle.rglob("*")
            if path.is_file()
        }
        indexed = run(
            binary,
            "registry",
            "index",
            "--bundle",
            bundle,
            "--database",
            database,
            "--verbose",
        )
        require(indexed.returncode == 0, "valid import failed", indexed)
        require("records=3" in indexed.stdout, "import count missing", indexed)
        require(
            "[registry-index] transaction_commit" in indexed.stderr,
            "bounded verbose phase missing",
            indexed,
        )
        after = {
            path.relative_to(bundle): sha256(path.read_bytes())
            for path in bundle.rglob("*")
            if path.is_file()
        }
        require(before == after, "source bundle changed during import")

        duplicate = run(
            binary, "registry", "index", "--bundle", bundle, "--database", database
        )
        require(duplicate.returncode == 0, "idempotent import failed", duplicate)
        require(
            "registry snapshot already indexed" in duplicate.stdout,
            "idempotent result missing",
            duplicate,
        )

        connection = sqlite3.connect(database)
        require(
            connection.execute("SELECT schema_version FROM schema_info").fetchone()
            == (3,),
            "schema version is wrong",
        )
        require(
            connection.execute("PRAGMA foreign_key_check").fetchall() == [],
            "foreign-key violations found",
        )
        require(
            connection.execute(
                "SELECT argument_value FROM package_arguments ORDER BY position"
            ).fetchall()
            == [("serve",), ("--safe",), (None,)],
            "package argument ordering changed",
        )
        connection.close()

        summary = run(binary, "registry", "summarize", database)
        require(summary.returncode == 0, "summary failed", summary)
        for expected in (
            "records=3",
            "unique_server_names=2",
            "with_repository=1",
            "package_only=1",
            "remote_only=1",
            "package_and_remote=1",
            "status.active=2",
            "status.deprecated=1",
        ):
            require(expected in summary.stdout, f"summary missing {expected}", summary)
        summary_json = run(
            binary, "registry", "summarize", database, "--format", "json"
        )
        require(
            json.loads(summary_json.stdout)["records"] == 3,
            "summary JSON is invalid",
            summary_json,
        )

        exact = run(
            binary, "registry", "search", database, "io.example/alpha", "--format", "jsonl"
        )
        exact_rows = [json.loads(line) for line in exact.stdout.splitlines()]
        require(
            {row["server_version"] for row in exact_rows} == {"1.0.0", "2.0.0"},
            "exact search did not return every version",
            exact,
        )
        exact_repeat = run(
            binary, "registry", "search", database, "io.example/alpha", "--format", "jsonl"
        )
        require(
            exact_repeat.stdout == exact.stdout,
            "exact search ordering is unstable",
            exact_repeat,
        )
        for query in ("GitHub", "@example/alpha", "remote.example"):
            found = run(binary, "registry", "search", database, query)
            require(found.returncode == 0 and found.stdout, f"search failed for {query}", found)
        injection = run(binary, "registry", "search", database, "' OR 1=1 --")
        require(injection.returncode == 0, "bound SQL-injection query failed", injection)
        filtered_search = run(
            binary,
            "registry",
            "search",
            database,
            "alpha",
            "--status",
            "deprecated",
            "--transport",
            "streamable-http",
            "--package-registry",
            "pypi",
            "--format",
            "jsonl",
        )
        require(
            [json.loads(line)["server_version"] for line in filtered_search.stdout.splitlines()]
            == ["2.0.0"],
            "combined search filters failed",
            filtered_search,
        )

        listed = run(
            binary,
            "registry",
            "list",
            database,
            "--has-remote",
            "--without-repository",
            "--limit",
            "1",
            "--offset",
            "1",
            "--format",
            "jsonl",
        )
        require(
            len([json.loads(line) for line in listed.stdout.splitlines()]) == 1,
            "list limit/offset or JSONL failed",
            listed,
        )
        contradictory = run(
            binary,
            "registry",
            "list",
            database,
            "--has-repository",
            "--without-repository",
        )
        require(contradictory.returncode == 1, "contradictory filters accepted")
        too_many = run(binary, "registry", "list", database, "--limit", "1001")
        require(too_many.returncode == 8, "maximum list limit not enforced", too_many)
        offset_overflow = run(
            binary,
            "registry",
            "list",
            database,
            "--offset",
            "18446744073709551615",
        )
        require(
            offset_overflow.returncode in (1, 8),
            "numeric offset overflow was accepted",
            offset_overflow,
        )
        for arguments, expected_names in (
            (("--status", "deprecated"), {"io.example/alpha"}),
            (("--transport", "sse"), {"io.example/alpha", "io.example/beta"}),
            (("--package-registry", "pypi"), {"io.example/alpha"}),
            (("--repository-host", "github.com"), {"io.example/alpha"}),
            (("--remote-host", "remote.example"), {"io.example/beta"}),
        ):
            filtered = run(
                binary,
                "registry",
                "list",
                database,
                *arguments,
                "--format",
                "jsonl",
            )
            rows = [json.loads(line) for line in filtered.stdout.splitlines()]
            require(
                {row["server_identifier"] for row in rows} == expected_names,
                f"list filter {arguments} failed",
                filtered,
            )

        shown = run(binary, "registry", "show", database, "io.example/alpha")
        require(
            shown.stdout.find("server_version=1.0.0")
            < shown.stdout.find("server_version=2.0.0"),
            "show version byte ordering is unstable",
            shown,
        )
        require("canonical=" not in shown.stdout, "canonical leaked by default", shown)
        shown_json = run(
            binary,
            "registry",
            "show",
            database,
            "io.example/alpha",
            "--version",
            "1.0.0",
            "--include-canonical",
            "--format",
            "json",
        )
        parsed_show = json.loads(shown_json.stdout)
        require(
            parsed_show[0]["canonical"]["server_identifier"] == "io.example/alpha",
            "included canonical JSON is invalid",
            shown_json,
        )
        remote_show = json.loads(
            run(
                binary,
                "registry",
                "show",
                database,
                "io.example/alpha",
                "--version",
                "2.0.0",
                "--format",
                "json",
            ).stdout
        )
        require(
            [remote["url"] for remote in remote_show[0]["remotes"]]
            == [
                "https://alpha.example:8443/mcp",
                "https://alpha.example/events",
            ],
            "remote ordering changed",
        )
        missing = run(binary, "registry", "show", database, "io.example/missing")
        require(missing.returncode == 5, "missing server category is wrong", missing)

        second_bundle = root / "second"
        second_digest = make_bundle(
            second_bundle,
            specs,
            "2026-01-04T00:00:00Z",
            observed_at="2026-01-02T00:00:00Z",
        )
        second = run(
            binary,
            "registry",
            "index",
            "--bundle",
            second_bundle,
            "--database",
            database,
        )
        require(second.returncode == 0, "second snapshot import failed", second)
        latest = json.loads(
            run(binary, "registry", "summarize", database, "--format", "json").stdout
        )
        require(
            latest["snapshot_sha256"] == second_digest,
            "latest snapshot selection is wrong",
        )
        tie_bundle = root / "tie"
        tie_digest = make_bundle(
            tie_bundle,
            specs,
            "2026-01-04T00:00:00Z",
            observed_at="2026-01-03T00:00:00Z",
        )
        tie = run(
            binary,
            "registry",
            "index",
            "--bundle",
            tie_bundle,
            "--database",
            database,
        )
        require(tie.returncode == 0, "tie-break snapshot import failed", tie)
        tied_latest = json.loads(
            run(binary, "registry", "summarize", database, "--format", "json").stdout
        )
        require(
            tied_latest["snapshot_sha256"] == tie_digest,
            "latest snapshot ID tie-breaker is wrong",
        )
        explicit = json.loads(
            run(
                binary,
                "registry",
                "summarize",
                database,
                "--snapshot",
                digest,
                "--format",
                "json",
            ).stdout
        )
        require(explicit["snapshot_sha256"] == digest, "explicit snapshot failed")
        connection = sqlite3.connect(database)
        require(
            connection.execute("SELECT COUNT(*) FROM server_versions").fetchone() == (3,),
            "exact server versions were not reused across snapshots",
        )
        connection.close()

        fallback_database = root / "fallback.sqlite"
        shutil.copy2(database, fallback_database)
        connection = sqlite3.connect(fallback_database)
        connection.execute("UPDATE schema_info SET search_mode='like'")
        connection.commit()
        connection.close()
        fallback = run(
            binary,
            "registry",
            "search",
            fallback_database,
            "GitHub",
            "--format",
            "jsonl",
        )
        require(
            fallback.returncode == 0
            and json.loads(fallback.stdout)["server_identifier"] == "io.example/alpha",
            "bounded LIKE fallback failed",
            fallback,
        )

        conflict_specs = json.loads(json.dumps(specs))
        conflict_specs[0]["description"] = "conflicting immutable content"
        conflict_bundle = root / "conflict"
        make_bundle(conflict_bundle, conflict_specs, "2026-01-05T00:00:00Z")
        conflict = run(
            binary,
            "registry",
            "index",
            "--bundle",
            conflict_bundle,
            "--database",
            database,
        )
        require(
            conflict.returncode == 0
            and "changed_identity_records=1" in conflict.stdout,
            "changed canonical variant was rejected",
            conflict,
        )
        connection = sqlite3.connect(database)
        require(
            connection.execute("SELECT COUNT(*) FROM snapshots").fetchone() == (4,)
            and connection.execute(
                "SELECT COUNT(*) FROM server_versions "
                "WHERE server_identifier=? AND server_version=?",
                (specs[0]["name"], specs[0]["version"]),
            ).fetchone()
            == (2,),
            "changed canonical variant was not preserved",
        )
        connection.close()

        missing_success = root / "missing-success"
        shutil.copytree(bundle, missing_success)
        (missing_success / "_SUCCESS").unlink()
        rejected = run(
            binary,
            "registry",
            "index",
            "--bundle",
            missing_success,
            "--database",
            root / "rejected.sqlite",
        )
        require(rejected.returncode == 3, "missing _SUCCESS was accepted", rejected)

        invalid_bundle = root / "invalid-bundle"
        shutil.copytree(bundle, invalid_bundle)
        (invalid_bundle / "raw" / "page-000001.json").write_bytes(b"changed")
        invalid = run(
            binary,
            "registry",
            "index",
            "--bundle",
            invalid_bundle,
            "--database",
            root / "invalid.sqlite",
        )
        require(invalid.returncode == 3, "invalid artifact was accepted", invalid)

        malformed_manifest = root / "malformed-manifest"
        shutil.copytree(bundle, malformed_manifest)
        (malformed_manifest / "manifest.json").write_text("{bad")
        malformed_manifest_result = run(
            binary,
            "registry",
            "index",
            "--bundle",
            malformed_manifest,
            "--database",
            root / "malformed-manifest.sqlite",
        )
        require(
            malformed_manifest_result.returncode == 3,
            "malformed manifest was accepted",
            malformed_manifest_result,
        )

        malformed_canonical = root / "malformed-canonical"
        shutil.copytree(bundle, malformed_canonical)
        (malformed_canonical / "canonical" / "servers.jsonl").write_bytes(b"{bad\n")
        refresh_canonical_metadata(malformed_canonical, 1)
        malformed_canonical_db = root / "malformed-canonical.sqlite"
        malformed_canonical_result = run(
            binary,
            "registry",
            "index",
            "--bundle",
            malformed_canonical,
            "--database",
            malformed_canonical_db,
        )
        require(
            malformed_canonical_result.returncode == 7,
            "malformed canonical JSONL was accepted",
            malformed_canonical_result,
        )
        require(
            not malformed_canonical_db.exists(),
            "validation failure created a database",
        )

        hash_inconsistent = root / "hash-inconsistent"
        shutil.copytree(bundle, hash_inconsistent)
        hash_path = hash_inconsistent / "canonical" / "servers.jsonl"
        hash_records = [
            json.loads(line) for line in hash_path.read_text().splitlines()
        ]
        hash_records[0]["canonical_sha256"] = "0" * 64
        hash_path.write_text(
            "".join(encoded(record) + "\n" for record in hash_records)
        )
        refresh_canonical_metadata(hash_inconsistent)
        hash_database = root / "hash-inconsistent.sqlite"
        hash_result = run(
            binary,
            "registry",
            "index",
            "--bundle",
            hash_inconsistent,
            "--database",
            hash_database,
        )
        require(
            hash_result.returncode == 7
            and "canonical record content hash mismatch" in hash_result.stderr
            and not hash_database.exists(),
            "hash-inconsistent canonical record did not fail closed",
            hash_result,
        )

        count_mismatch = root / "count-mismatch"
        shutil.copytree(bundle, count_mismatch)
        manifest_path = count_mismatch / "manifest.json"
        manifest = json.loads(manifest_path.read_text())
        manifest["counts"]["unique_server_versions"] = 4
        manifest_path.write_text(encoded(manifest) + "\n")
        mismatch = run(
            binary,
            "registry",
            "index",
            "--bundle",
            count_mismatch,
            "--database",
            root / "count-mismatch.sqlite",
        )
        require(mismatch.returncode == 7, "manifest count mismatch was accepted", mismatch)

        duplicate_bundle = root / "duplicate"
        shutil.copytree(bundle, duplicate_bundle)
        canonical_path = duplicate_bundle / "canonical" / "servers.jsonl"
        canonical_lines = canonical_path.read_bytes().splitlines(keepends=True)
        canonical_path.write_bytes(canonical_lines[0] + b"".join(canonical_lines))
        refresh_canonical_metadata(duplicate_bundle, 4)
        duplicate_result = run(
            binary,
            "registry",
            "index",
            "--bundle",
            duplicate_bundle,
            "--database",
            root / "duplicate.sqlite",
        )
        require(
            duplicate_result.returncode == 7,
            "duplicate exact identity was accepted",
            duplicate_result,
        )

        record_limited = run(
            binary,
            "registry",
            "index",
            "--bundle",
            bundle,
            "--database",
            root / "record-limit.sqlite",
            "--maximum-records",
            "2",
        )
        require(record_limited.returncode == 8, "record limit was not enforced")
        line_limited_db = root / "line-limit.sqlite"
        line_limited = run(
            binary,
            "registry",
            "index",
            "--bundle",
            bundle,
            "--database",
            line_limited_db,
            "--maximum-line-bytes",
            "20",
        )
        require(line_limited.returncode == 7, "line limit was not enforced", line_limited)
        size_limited = run(
            binary,
            "registry",
            "index",
            "--bundle",
            bundle,
            "--database",
            root / "size-limit.sqlite",
            "--maximum-database-bytes",
            "1",
        )
        require(size_limited.returncode == 9, "database size limit was not enforced")

        incompatible = root / "incompatible.sqlite"
        connection = sqlite3.connect(incompatible)
        connection.execute(
            "CREATE TABLE schema_info("
            "schema_version INTEGER,created_by_version TEXT,search_mode TEXT)"
        )
        connection.execute("INSERT INTO schema_info VALUES(99,'test','like')")
        connection.commit()
        connection.close()
        incompatible_result = run(
            binary,
            "registry",
            "summarize",
            incompatible,
        )
        require(
            incompatible_result.returncode == 6,
            "incompatible schema version was accepted",
            incompatible_result,
        )
    finally:
        shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    main()
