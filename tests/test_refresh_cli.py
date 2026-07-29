#!/usr/bin/env python3
"""Offline periodic Registry refresh tests."""

from datetime import datetime, timedelta
import http.server
import json
import os
import pathlib
import re
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import threading
import urllib.parse


BASE_ENTRY = {
    "server": {
        "name": "io.example/baseline",
        "version": "1.0.0",
        "description": "baseline",
    },
    "_meta": {
        "io.modelcontextprotocol.registry/official": {
            "isLatest": True,
            "publishedAt": "2026-07-20T00:00:00Z",
            "updatedAt": "2026-07-20T00:00:00Z",
            "status": "active",
        }
    },
}


def entry(name, version="1.0.0", description="changed"):
    value = json.loads(json.dumps(BASE_ENTRY))
    value["server"].update(
        {"name": name, "version": version, "description": description}
    )
    return value


def metadata_entry(**changes):
    value = json.loads(json.dumps(BASE_ENTRY))
    value["_meta"]["io.modelcontextprotocol.registry/official"].update(changes)
    return value


class Handler(http.server.BaseHTTPRequestHandler):
    requests = []

    def log_message(self, _format, *_args):
        pass

    def reply(self, servers, cursor=None, status=200):
        body = json.dumps(
            {"servers": servers, "metadata": {"nextCursor": cursor}},
            separators=(",", ":"),
        ).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        parsed = urllib.parse.urlsplit(self.path)
        query = urllib.parse.parse_qs(parsed.query)
        Handler.requests.append((parsed.path, query, self.path))
        if parsed.path == "/baseline/v0.1/servers":
            self.reply([BASE_ENTRY])
        elif parsed.path == "/paged/v0.1/servers" and "cursor" not in query:
            self.reply([entry("io.example/new-one")], "second")
        elif parsed.path == "/paged/v0.1/servers":
            self.reply([entry("io.example/new-two")])
        elif parsed.path == "/mixed/v0.1/servers":
            self.reply([BASE_ENTRY, entry("io.example/new")])
        elif parsed.path == "/empty/v0.1/servers":
            self.reply([])
        elif parsed.path == "/deprecated/v0.1/servers":
            self.reply([metadata_entry(status="deprecated")])
        elif parsed.path == "/deleted/v0.1/servers":
            self.reply([metadata_entry(status="deleted")])
        elif parsed.path == "/status-message/v0.1/servers":
            self.reply([metadata_entry(statusMessage="scheduled retirement")])
        elif parsed.path == "/is-latest/v0.1/servers":
            self.reply([metadata_entry(isLatest=False)])
        elif parsed.path == "/conflict/v0.1/servers":
            self.reply([entry("io.example/baseline", description="conflict")])
        elif parsed.path == "/interrupt/v0.1/servers" and "cursor" not in query:
            self.reply([entry("io.example/committed")], "resume-here")
        elif parsed.path == "/interrupt/v0.1/servers":
            self.reply([], status=500)
        elif parsed.path == "/resume/v0.1/servers":
            self.reply([entry("io.example/resumed")])
        else:
            self.reply([], status=404)


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


def create_baseline(binary, base, root, name):
    bundle = root / f"{name}-bundle"
    database = root / f"{name}.sqlite"
    collected = run(
        binary,
        "registry",
        "collect",
        "--registry-base-url",
        base + "/baseline",
        "--output",
        bundle,
    )
    require(collected.returncode == 0, "baseline collection failed", collected)
    indexed = run(
        binary,
        "registry",
        "index",
        "--bundle",
        bundle,
        "--database",
        database,
    )
    require(indexed.returncode == 0, "baseline indexing failed", indexed)
    return bundle, database


def snapshot_count(database):
    connection = sqlite3.connect(database)
    count = connection.execute("SELECT COUNT(*) FROM snapshots").fetchone()[0]
    connection.close()
    return count


def main():
    binary = os.path.abspath(sys.argv[1])
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    root = pathlib.Path(tempfile.mkdtemp(prefix="mcpo-refresh-test-"))
    try:
        base = f"http://127.0.0.1:{server.server_port}"

        no_database = root / "missing.sqlite"
        no_output = root / "must-not-exist"
        before_requests = len(Handler.requests)
        missing = run(
            binary,
            "registry",
            "refresh",
            "--database",
            no_database,
            "--registry-base-url",
            base + "/empty",
            "--output",
            no_output,
        )
        require(
            missing.returncode != 0
            and "no_baseline_snapshot" in missing.stderr,
            "missing baseline category is wrong",
            missing,
        )
        require(
            len(Handler.requests) == before_requests
            and not no_output.exists()
            and not no_database.exists(),
            "missing baseline changed state or contacted the network",
        )

        _, database = create_baseline(binary, base, root, "derived")
        empty_catalog = root / "empty-catalog.sqlite"
        shutil.copy2(database, empty_catalog)
        connection = sqlite3.connect(empty_catalog)
        connection.execute("DELETE FROM snapshots")
        connection.commit()
        connection.close()
        empty_catalog_output = root / "empty-catalog-output"
        before_requests = len(Handler.requests)
        empty_catalog_result = run(
            binary,
            "registry",
            "refresh",
            "--database",
            empty_catalog,
            "--registry-base-url",
            base + "/empty",
            "--output",
            empty_catalog_output,
        )
        require(
            empty_catalog_result.returncode != 0
            and "no_baseline_snapshot" in empty_catalog_result.stderr
            and len(Handler.requests) == before_requests
            and not empty_catalog_output.exists(),
            "empty catalog did not fail before collection",
            empty_catalog_result,
        )
        malformed_output = root / "malformed"
        malformed = run(
            binary,
            "registry",
            "refresh",
            "--database",
            database,
            "--registry-base-url",
            base + "/empty",
            "--output",
            malformed_output,
            "--updated-since",
            "2026-07-26T18:42Z",
        )
        require(
            malformed.returncode != 0
            and "invalid_updated_since" in malformed.stderr
            and not malformed_output.exists(),
            "malformed updated_since was accepted",
            malformed,
        )

        connection = sqlite3.connect(database)
        baseline_completed_at = connection.execute(
            "SELECT completed_at FROM snapshots ORDER BY id LIMIT 1"
        ).fetchone()[0]
        tied_completed_at = (
            datetime.fromisoformat(baseline_completed_at.replace("Z", "+00:00"))
            + timedelta(seconds=1)
        ).strftime("%Y-%m-%dT%H:%M:%SZ")
        template = connection.execute(
            "SELECT started_at,registry_base_url,collector_name,collector_version,"
            "collector_git_commit,bundle_version,source_bundle_path,pages,"
            "records_received,unique_server_versions,imported_at "
            "FROM snapshots ORDER BY id LIMIT 1"
        ).fetchone()
        for digest in ("a" * 64, "b" * 64):
            connection.execute(
                "INSERT INTO snapshots(snapshot_sha256,completed_at,started_at,"
                "registry_base_url,collector_name,collector_version,"
                "collector_git_commit,bundle_version,source_bundle_path,pages,"
                "records_received,unique_server_versions,imported_at)"
                " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)",
                (digest, tied_completed_at, *template),
            )
        connection.commit()
        connection.close()

        Handler.requests.clear()
        derived_output = root / "derived-refresh"
        derived = run(
            binary,
            "registry",
            "refresh",
            "--database",
            database,
            "--registry-base-url",
            base + "/paged",
            "--output",
            derived_output,
            "--format",
            "json",
        )
        require(derived.returncode == 0, "derived refresh failed", derived)
        summary = json.loads(derived.stdout)
        require(
            summary["updated_since"] == tied_completed_at
            and summary["base_snapshot_sha256"] == "b" * 64,
            "latest snapshot or ID tie-break selection is wrong",
            derived,
        )
        require(
            summary["completed_pages"] == 2
            and summary["received_records"] == 2
            and all(
                request[1].get("updated_since")
                == [tied_completed_at]
                for request in Handler.requests
            )
            and Handler.requests[1][1].get("cursor") == ["second"],
            "incremental filter was not retained across pagination",
            derived,
        )
        manifest = json.loads((derived_output / "manifest.json").read_text())
        require(
            manifest["bundle_version"] == 2
            and manifest["collection_mode"] == "incremental"
            and manifest["updated_since"] == tied_completed_at
            and manifest["base_snapshot_sha256"] == "b" * 64,
            "incremental manifest provenance is wrong",
        )
        validated = run(binary, "bundle", "validate", derived_output)
        require(validated.returncode == 0, "incremental bundle is invalid", validated)

        _, mixed_database = create_baseline(binary, base, root, "mixed")
        Handler.requests.clear()
        mixed_output = root / "mixed-refresh"
        override = "2026-07-26T18:42:11Z"
        mixed = run(
            binary,
            "registry",
            "refresh",
            "--database",
            mixed_database,
            "--registry-base-url",
            base + "/mixed",
            "--output",
            mixed_output,
            "--updated-since",
            override,
            "--format",
            "json",
        )
        require(mixed.returncode == 0, "override refresh failed", mixed)
        mixed_summary = json.loads(mixed.stdout)
        require(
            mixed_summary["updated_since"] == override
            and mixed_summary["inserted_server_versions"] == 1
            and mixed_summary["reused_server_versions"] == 1
            and mixed_summary["changed_identity_records"] == 0
            and mixed_summary["snapshot_links_created"] == 2
            and Handler.requests[0][1].get("updated_since") == [override]
            and "updated_since=2026-07-26T18%3A42%3A11Z"
            in Handler.requests[0][2],
            "override, URL encoding, or import counts are wrong",
            mixed,
        )
        before_duplicate = snapshot_count(mixed_database)
        duplicate = run(
            binary,
            "registry",
            "index",
            "--bundle",
            mixed_output,
            "--database",
            mixed_database,
        )
        require(
            duplicate.returncode == 0
            and snapshot_count(mixed_database) == before_duplicate,
            "incremental bundle re-import is not idempotent",
            duplicate,
        )
        connection = sqlite3.connect(mixed_database)
        baseline_links = connection.execute(
            "SELECT COUNT(*) FROM snapshot_server_versions WHERE snapshot_id=1"
        ).fetchone()[0]
        baseline_status = connection.execute(
            "SELECT registry_status FROM server_versions "
            "WHERE server_identifier='io.example/baseline'"
        ).fetchone()[0]
        connection.close()
        require(
            baseline_links == 1 and baseline_status == "active",
            "prior records changed or absence implied removal",
        )

        _, empty_database = create_baseline(binary, base, root, "empty")
        empty_output = root / "empty-refresh"
        empty = run(
            binary,
            "registry",
            "refresh",
            "--database",
            empty_database,
            "--registry-base-url",
            base + "/empty",
            "--output",
            empty_output,
            "--updated-since",
            override,
            "--format",
            "json",
        )
        empty_summary = json.loads(empty.stdout)
        require(
            empty.returncode == 0
            and empty_summary["received_records"] == 0
            and empty_summary["inserted_server_versions"] == 0
            and empty_summary["reused_server_versions"] == 0
            and empty_summary["changed_identity_records"] == 0
            and empty_summary["snapshot_links_created"] == 0
            and snapshot_count(empty_database) == 2,
            "empty incremental result failed",
            empty,
        )

        mutation_cases = (
            ("deprecated", "deprecated"),
            ("deleted", "deleted"),
            ("status-message", "active"),
            ("is-latest", "active"),
        )
        mutation_results = {}
        for case_name, expected_status in mutation_cases:
            _, mutation_database = create_baseline(
                binary, base, root, f"mutation-{case_name}"
            )
            mutation_output = root / f"mutation-{case_name}-refresh"
            arguments = [
                "registry",
                "refresh",
                "--database",
                mutation_database,
                "--registry-base-url",
                base + f"/{case_name}",
                "--output",
                mutation_output,
                "--updated-since",
                override,
            ]
            if case_name != "deprecated":
                arguments.extend(["--format", "json"])
            mutation = run(binary, *arguments)
            require(
                mutation.returncode == 0
                and "identity_digest_conflict" not in mutation.stderr,
                f"{case_name} metadata mutation failed",
                mutation,
            )
            if case_name == "deprecated":
                require(
                    "changed_identity_records=1\n" in mutation.stdout
                    and "inserted_server_versions=1\n" in mutation.stdout
                    and "reused_server_versions=0\n" in mutation.stdout,
                    "text refresh mutation counters are wrong",
                    mutation,
                )
            else:
                mutation_summary = json.loads(mutation.stdout)
                require(
                    mutation_summary["changed_identity_records"] == 1
                    and mutation_summary["inserted_server_versions"] == 1
                    and mutation_summary["reused_server_versions"] == 0,
                    f"{case_name} JSON refresh mutation counters are wrong",
                    mutation,
                )
            connection = sqlite3.connect(mutation_database)
            variants = connection.execute(
                "SELECT id,registry_status,canonical_sha256,canonical_json "
                "FROM server_versions "
                "WHERE server_identifier='io.example/baseline' "
                "AND server_version='1.0.0' ORDER BY id"
            ).fetchall()
            links = connection.execute(
                "SELECT snapshot_id,server_version_id "
                "FROM snapshot_server_versions ORDER BY snapshot_id"
            ).fetchall()
            connection.close()
            require(
                len(variants) == 2
                and variants[0][1] == "active"
                and variants[1][1] == expected_status
                and variants[0][2] != variants[1][2]
                and links == [(1, variants[0][0]), (2, variants[1][0])],
                f"{case_name} did not preserve and link immutable variants",
            )
            official = json.loads(variants[1][3])["original"]["_meta"][
                "io.modelcontextprotocol.registry/official"
            ]
            if case_name == "status-message":
                require(
                    official["status"] == "active"
                    and official["statusMessage"] == "scheduled retirement",
                    "statusMessage-only variant was not preserved",
                )
            if case_name == "is-latest":
                require(
                    official["status"] == "active"
                    and official["isLatest"] is False,
                    "isLatest-only variant was not preserved",
                )
            mutation_results[case_name] = (
                mutation_database,
                mutation_output,
                len(variants),
                len(links),
            )

        (
            idempotent_database,
            idempotent_bundle,
            before_variant_count,
            before_link_count,
        ) = mutation_results["deprecated"]
        before_snapshot_count = snapshot_count(idempotent_database)
        repeated = run(
            binary,
            "registry",
            "index",
            "--bundle",
            idempotent_bundle,
            "--database",
            idempotent_database,
        )
        connection = sqlite3.connect(idempotent_database)
        after_variant_count = connection.execute(
            "SELECT COUNT(*) FROM server_versions"
        ).fetchone()[0]
        after_link_count = connection.execute(
            "SELECT COUNT(*) FROM snapshot_server_versions"
        ).fetchone()[0]
        connection.close()
        require(
            repeated.returncode == 0
            and "registry snapshot already indexed" in repeated.stdout
            and snapshot_count(idempotent_database) == before_snapshot_count
            and after_variant_count == before_variant_count
            and after_link_count == before_link_count,
            "same mutation bundle re-import was not idempotent",
            repeated,
        )

        _, resume_database = create_baseline(binary, base, root, "resume")
        interrupted_output = root / "interrupted"
        interrupted = run(
            binary,
            "registry",
            "refresh",
            "--database",
            resume_database,
            "--registry-base-url",
            base + "/interrupt",
            "--output",
            interrupted_output,
            "--updated-since",
            override,
            "--maximum-attempts-per-page",
            "1",
        )
        require(interrupted.returncode != 0, "interruption unexpectedly succeeded")
        retained = re.search(r"failed bundle retained at ([^\s;]+)", interrupted.stderr)
        require(retained is not None, "retained partial path is missing", interrupted)
        partial = pathlib.Path(retained.group(1))
        checkpoint = json.loads((partial / "checkpoint.json").read_text())
        require(
            checkpoint["checkpoint_version"] == 3
            and checkpoint["completed_pages"] == 1
            and checkpoint["collection_mode"] == "incremental",
            "incremental checkpoint is not provenance-bound",
        )
        page_one_requests = sum(
            1
            for path, query, _ in Handler.requests
            if path == "/interrupt/v0.1/servers" and "cursor" not in query
        )
        resumed_output = root / "resumed"
        resumed = run(
            binary,
            "registry",
            "refresh",
            "--database",
            resume_database,
            "--registry-base-url",
            base + "/resume",
            "--output",
            resumed_output,
            "--resume",
            partial,
            "--updated-since",
            override,
        )
        require(
            resumed.returncode != 0,
            "registry URL provenance mismatch was accepted",
            resumed,
        )
        correct_resume_output = root / "resumed-correct"
        # The fixture's interrupt endpoint now succeeds for the committed cursor.
        original = Handler.do_GET

        def resume_get(self):
            parsed = urllib.parse.urlsplit(self.path)
            query = urllib.parse.parse_qs(parsed.query)
            if (
                parsed.path == "/interrupt/v0.1/servers"
                and query.get("cursor") == ["resume-here"]
            ):
                Handler.requests.append((parsed.path, query, self.path))
                self.reply([entry("io.example/resumed")])
                return
            original(self)

        Handler.do_GET = resume_get
        resumed = run(
            binary,
            "registry",
            "refresh",
            "--database",
            resume_database,
            "--registry-base-url",
            base + "/interrupt",
            "--output",
            correct_resume_output,
            "--resume",
            partial,
            "--updated-since",
            override,
        )
        require(resumed.returncode == 0, "incremental resume failed", resumed)
        require(
            sum(
                1
                for path, query, _ in Handler.requests
                if path == "/interrupt/v0.1/servers" and "cursor" not in query
            )
            == page_one_requests,
            "committed page was downloaded again",
        )
        base_mismatch_output = root / "resume-base-mismatch"
        before_base_mismatch_requests = len(Handler.requests)
        base_mismatch = run(
            binary,
            "registry",
            "refresh",
            "--database",
            resume_database,
            "--registry-base-url",
            base + "/interrupt",
            "--output",
            base_mismatch_output,
            "--resume",
            partial,
            "--updated-since",
            override,
        )
        require(
            base_mismatch.returncode != 0
            and "provenance mismatch" in base_mismatch.stderr
            and len(Handler.requests) == before_base_mismatch_requests,
            "different base snapshot digest was accepted on resume",
            base_mismatch,
        )
        mismatch_output = root / "resume-mismatch"
        before_mismatch_requests = len(Handler.requests)
        mismatch = run(
            binary,
            "registry",
            "refresh",
            "--database",
            resume_database,
            "--registry-base-url",
            base + "/interrupt",
            "--output",
            mismatch_output,
            "--resume",
            partial,
            "--updated-since",
            "2026-07-26T18:42:12Z",
        )
        require(
            mismatch.returncode != 0
            and "provenance mismatch" in mismatch.stderr
            and len(Handler.requests) == before_mismatch_requests,
            "different updated_since was accepted on resume",
            mismatch,
        )
        switched = run(
            binary,
            "registry",
            "collect",
            "--registry-base-url",
            base + "/interrupt",
            "--output",
            root / "switched",
            "--resume",
            partial,
        )
        require(
            switched.returncode != 0
            and "collection mode provenance mismatch" in switched.stderr,
            "incremental checkpoint was resumed as full",
            switched,
        )
    finally:
        server.shutdown()
        server.server_close()
        shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    main()
