#!/usr/bin/env python3
"""Offline CLI and HTTP-policy tests for the registry bundle milestone."""

import http.server
import json
import os
import pathlib
import select
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse


ENTRY = {
    "server": {
        "name": "io.example/fixture",
        "version": "1.0.0",
        "description": "fixture",
        "x-unknown": {"preserved": True},
    },
    "_meta": {
        "io.modelcontextprotocol.registry/official": {
            "publishedAt": "2026-07-25T10:00:00Z"
        }
    },
}


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, _format, *_args):
        pass

    def reply(self, status, body=b"", content_type="application/json", location=None):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        if location is not None:
            self.send_header("Location", location)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def registry(self, servers, cursor=None):
        return json.dumps(
            {"servers": servers, "metadata": {"nextCursor": cursor}},
            separators=(",", ":"),
        ).encode()

    def do_GET(self):
        parsed = urllib.parse.urlsplit(self.path)
        path = parsed.path
        query = urllib.parse.parse_qs(parsed.query)
        if path == "/same/v0.1/servers":
            self.reply(302, location="/same-target/v0.1/servers")
        elif path == "/same-target/v0.1/servers":
            self.reply(200, self.registry([ENTRY]))
        elif path == "/cross/v0.1/servers":
            self.reply(
                302,
                location=f"http://localhost:{self.server.server_port}/one/v0.1/servers",
            )
        elif path == "/redirect-loop/v0.1/servers":
            self.reply(302, location="/redirect-loop/v0.1/servers")
        elif path == "/slow/v0.1/servers":
            time.sleep(2)
            self.reply(200, self.registry([]))
        elif path == "/progress-slow/v0.1/servers":
            time.sleep(0.5)
            self.reply(200, self.registry([ENTRY]))
        elif path == "/partial/v0.1/servers" and "cursor" not in query:
            self.reply(200, self.registry([ENTRY], "next"))
        elif path == "/partial/v0.1/servers":
            self.reply(500, b'{"error":"fixture"}')
        elif path == "/pagination-cross/v0.1/servers" and "cursor" not in query:
            self.reply(200, self.registry([ENTRY], "next"))
        elif path == "/pagination-cross/v0.1/servers":
            self.reply(
                302,
                location=f"http://localhost:{self.server.server_port}/one/v0.1/servers",
            )
        elif path == "/oversized/v0.1/servers":
            self.reply(200, self.registry([ENTRY]) + b" " * 8192)
        elif path in ("/one/v0.1/servers", "/env/v0.1/servers"):
            self.reply(200, self.registry([ENTRY]))
        elif path == "/resume/v0.1/servers" and query.get("cursor") == ["cursor-three"]:
            final = json.loads(json.dumps(ENTRY))
            final["server"]["name"] = "io.example/resumed"
            self.reply(200, self.registry([final]))
        else:
            self.reply(404, b'{"error":"unexpected fixture path"}')


def run(binary, *arguments, env=None):
    return subprocess.run(
        [binary, *arguments],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=15,
        env=env,
        check=False,
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
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    root = pathlib.Path(tempfile.mkdtemp(prefix="mcpo-cli-test-"))
    try:
        base = f"http://127.0.0.1:{server.server_port}"

        one = root / "one"
        result = run(
            binary,
            "registry",
            "collect",
            "--registry-base-url",
            base + "/one",
            "--output",
            str(one),
        )
        require(result.returncode == 0, "local fixture collection failed", result)
        require((one / "_SUCCESS").is_file(), "successful fixture lacks _SUCCESS")

        validated = run(binary, "bundle", "validate", str(one))
        require(validated.returncode == 0, "bundle validate failed", validated)

        progress_bundle = root / "progress-slow"
        progress_process = subprocess.Popen(
            [
                binary,
                "registry",
                "collect",
                "--registry-base-url",
                base + "/progress-slow",
                "--output",
                str(progress_bundle),
                "--verbose",
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )
        observed_progress = []
        progress_deadline = time.monotonic() + 2.0
        saw_request_start = False
        while time.monotonic() < progress_deadline and not saw_request_start:
            readable, _, _ = select.select(
                [progress_process.stderr],
                [],
                [],
                max(0.0, progress_deadline - time.monotonic()),
            )
            if not readable:
                break
            chunk = os.read(progress_process.stderr.fileno(), 4096).decode()
            if not chunk:
                break
            observed_progress.append(chunk)
            saw_request_start = "request_start" in chunk
        require(saw_request_start, "request_start was not observable before completion")
        require(
            progress_process.poll() is None,
            "progress was only observable after collection completed",
        )
        progress_stdout, progress_stderr = progress_process.communicate(timeout=15)
        observed_progress.append(progress_stderr)
        progress_result = subprocess.CompletedProcess(
            progress_process.args,
            progress_process.returncode,
            progress_stdout,
            "".join(observed_progress),
        )
        require(
            progress_result.returncode == 0,
            "verbose progress fixture collection failed",
            progress_result,
        )
        require(
            "[registry]" not in progress_stdout
            and progress_stdout.startswith("registry collection complete:"),
            "verbose progress contaminated stdout",
            progress_result,
        )
        require(
            "[registry] success output=" in progress_result.stderr,
            "verbose success summary missing from stderr",
            progress_result,
        )

        same = root / "same"
        result = run(
            binary,
            "registry",
            "collect",
            "--registry-base-url",
            base + "/same",
            "--output",
            str(same),
        )
        require(result.returncode == 0, "same-origin redirect failed", result)

        cross = root / "cross"
        result = run(
            binary,
            "registry",
            "collect",
            "--registry-base-url",
            base + "/cross",
            "--output",
            str(cross),
        )
        require(result.returncode != 0, "cross-origin redirect was accepted", result)
        require(not (cross / "_SUCCESS").exists(), "cross-origin failure has _SUCCESS")

        loop = root / "loop"
        result = run(
            binary,
            "registry",
            "collect",
            "--registry-base-url",
            base + "/redirect-loop",
            "--maximum-redirects",
            "1",
            "--output",
            str(loop),
        )
        require(result.returncode != 0, "redirect limit was not enforced", result)

        slow = root / "slow"
        result = run(
            binary,
            "registry",
            "collect",
            "--registry-base-url",
            base + "/slow",
            "--request-timeout-seconds",
            "1",
            "--run-timeout-seconds",
            "1",
            "--output",
            str(slow),
        )
        require(result.returncode != 0, "request/run timeout was not enforced", result)

        partial = root / "partial"
        result = run(
            binary,
            "registry",
            "collect",
            "--registry-base-url",
            base + "/partial",
            "--output",
            str(partial),
        )
        require(result.returncode != 0, "partial fetch failure succeeded", result)
        require(not (partial / "_SUCCESS").exists(), "partial fetch has _SUCCESS")

        pagination_cross = root / "pagination-cross"
        result = run(
            binary,
            "registry",
            "collect",
            "--registry-base-url",
            base + "/pagination-cross",
            "--output",
            str(pagination_cross),
        )
        require(
            result.returncode != 0,
            "pagination response redirected a future request across origin",
            result,
        )

        oversized = root / "oversized"
        result = run(
            binary,
            "registry",
            "collect",
            "--registry-base-url",
            base + "/oversized",
            "--maximum-page-bytes",
            "100",
            "--output",
            str(oversized),
        )
        require(result.returncode != 0, "oversized page succeeded", result)

        environment = os.environ.copy()
        environment["MCPO_REGISTRY_BASE_URL"] = base + "/env"
        env_bundle = root / "env"
        result = run(
            binary, "registry", "collect", "--output", str(env_bundle), env=environment
        )
        require(result.returncode == 0, "environment URL was not used", result)

        environment["MCPO_REGISTRY_BASE_URL"] = "http://non-local.invalid"
        override = root / "override"
        result = run(
            binary,
            "registry",
            "collect",
            "--registry-base-url",
            base + "/one",
            "--output",
            str(override),
            env=environment,
        )
        require(result.returncode == 0, "CLI URL did not override environment", result)

        first = (one / "canonical" / "servers.jsonl").read_bytes()
        second = (override / "canonical" / "servers.jsonl").read_bytes()
        require(first == second, "identical inputs changed canonical bytes")
        first_manifest = json.loads((one / "manifest.json").read_text())
        second_manifest = json.loads((override / "manifest.json").read_text())
        require(
            first_manifest["snapshot_sha256"] == second_manifest["snapshot_sha256"],
            "identical input changed snapshot hash",
        )
        first_record = json.loads(first)
        second_record = json.loads(second)
        require(
            first_record["canonical_sha256"] == second_record["canonical_sha256"],
            "retrieval URL changed canonical record hash",
        )
        raw_metadata = [
            json.loads(line)
            for line in (one / "raw" / "pages.jsonl").read_text().splitlines()
        ]
        require(len(raw_metadata) == 1, "raw page metadata count is wrong")
        require(
            raw_metadata[0]["effective_response_url"]
            == base + "/one/v0.1/servers",
            "effective URL was not normalized",
        )
        require(
            "@" not in raw_metadata[0]["effective_response_url"],
            "raw effective URL contains credentials",
        )

        legacy = root / "legacy-cli"
        (legacy / "raw").mkdir(parents=True)
        first_legacy = json.loads(json.dumps(ENTRY))
        first_legacy["server"]["name"] = "io.example/legacy-one"
        second_legacy = json.loads(json.dumps(ENTRY))
        second_legacy["server"]["name"] = "io.example/legacy-two"
        (legacy / "raw" / "page-000001.json").write_text(
            json.dumps(
                {
                    "servers": [first_legacy],
                    "metadata": {"nextCursor": "cursor-two"},
                },
                separators=(",", ":"),
            )
        )
        (legacy / "raw" / "page-000002.json").write_text(
            json.dumps(
                {
                    "servers": [second_legacy],
                    "metadata": {
                        "cursor": "cursor-two",
                        "nextCursor": "cursor-three",
                    },
                },
                separators=(",", ":"),
            )
        )
        reconstructed = run(
            binary,
            "registry",
            "checkpoint",
            "reconstruct",
            str(legacy),
            "--registry-base-url",
            base + "/resume",
        )
        require(
            reconstructed.returncode == 0,
            "CLI checkpoint reconstruction failed",
            reconstructed,
        )
        checkpoint = json.loads((legacy / "checkpoint.json").read_text())
        require(checkpoint["completed_pages"] == 2, "checkpoint page count is wrong")
        require(checkpoint["records_received"] == 2, "checkpoint record count is wrong")
        require(checkpoint["next_cursor"] == "cursor-three", "checkpoint cursor is wrong")
        require(not (legacy / "_SUCCESS").exists(), "reconstruction created _SUCCESS")

        resumed = root / "resumed"
        result = run(
            binary,
            "registry",
            "collect",
            "--registry-base-url",
            base + "/resume",
            "--resume",
            str(legacy),
            "--output",
            str(resumed),
        )
        require(result.returncode == 0, "CLI resume collection failed", result)
        require(
            run(binary, "bundle", "validate", str(resumed)).returncode == 0,
            "resumed CLI bundle did not validate",
        )
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=3)
        shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    main()
