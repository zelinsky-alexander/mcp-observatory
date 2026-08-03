#!/usr/bin/env python3
"""Offline reliability tests for catalog maintenance orchestration."""

import fcntl
import hashlib
import http.server
import json
import os
from pathlib import Path
import shutil
import signal
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
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


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, _format, *_args):
        pass

    def do_GET(self):
        parsed = urllib.parse.urlsplit(self.path)
        if parsed.path == "/baseline/v0.1/servers":
            entries = [BASE_ENTRY]
        elif parsed.path == "/refresh/v0.1/servers":
            changed = json.loads(json.dumps(BASE_ENTRY))
            changed["server"].update(
                {"name": "io.example/incremental", "description": "new"}
            )
            entries = [changed]
        else:
            self.send_error(404)
            return
        body = json.dumps(
            {"servers": entries, "metadata": {"nextCursor": None}},
            separators=(",", ":"),
        ).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def run(command, *, env=None, timeout=30):
    return subprocess.run(
        [str(value) for value in command],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
        timeout=timeout,
        env=env,
    )


def require(condition, message, result=None):
    if condition:
        return
    detail = ""
    if result is not None:
        detail = f"\nstdout: {result.stdout}\nstderr: {result.stderr}"
    raise AssertionError(message + detail)


def file_digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def snapshot_count(path):
    with sqlite3.connect(path) as connection:
        return connection.execute("SELECT COUNT(*) FROM snapshots").fetchone()[0]


def create_baseline(binary, root, base):
    bundle = root / "baseline-bundle"
    database = root / "catalog.sqlite"
    collected = run(
        [
            binary,
            "registry",
            "collect",
            "--registry-base-url",
            base + "/baseline",
            "--output",
            bundle,
            "--maximum-attempts-per-page",
            "1",
        ]
    )
    require(collected.returncode == 0, "baseline collection failed", collected)
    indexed = run(
        [binary, "registry", "index", "--bundle", bundle, "--database", database]
    )
    require(indexed.returncode == 0, "baseline import failed", indexed)
    return bundle, database


def maintenance_command(tool, binary, database, runtime, base):
    return [
        sys.executable,
        tool,
        "refresh",
        "--binary",
        binary,
        "--database",
        database,
        "--runtime-dir",
        runtime,
        "--timezone",
        "UTC",
        "--",
        "--registry-base-url",
        base + "/refresh",
        "--maximum-attempts-per-page",
        "1",
        "--run-timeout-seconds",
        "10",
    ]


def make_wrapper(root, binary):
    wrapper = root / "test-binary"
    wrapper.write_text(
        """#!/usr/bin/env python3
import os
import subprocess
import sys
import time

real = os.environ["MCPO_TEST_REAL_BINARY"]
mode = os.environ.get("MCPO_TEST_MODE", "pass")
if sys.argv[1:3] == ["registry", "refresh"]:
    if mode == "fail":
        print("injected collection failure", file=sys.stderr)
        raise SystemExit(47)
    if mode == "sleep":
        time.sleep(20)
    result = subprocess.run([real, *sys.argv[1:]], check=False)
    if mode == "corrupt" and result.returncode == 0:
        database = sys.argv[sys.argv.index("--database") + 1]
        with open(database, "wb") as output:
            output.write(b"not sqlite")
    raise SystemExit(result.returncode)
os.execv(real, [real, *sys.argv[1:]])
""",
        encoding="utf-8",
    )
    wrapper.chmod(0o700)
    return wrapper


def main():
    binary = Path(sys.argv[1]).resolve()
    tool = Path(sys.argv[2]).resolve()
    root = Path(tempfile.mkdtemp(prefix="mcpo-maintenance-test-"))
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        base = f"http://127.0.0.1:{server.server_port}"
        bundle, database = create_baseline(binary, root, base)
        original_digest = file_digest(database)

        # Native writers and maintenance use the same serialized lock.
        lock_path = Path(f"{database}.writer.lock")
        with lock_path.open("a+b") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            waiting = subprocess.Popen(
                [
                    binary,
                    "registry",
                    "index",
                    "--bundle",
                    bundle,
                    "--database",
                    database,
                ],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            time.sleep(0.3)
            require(waiting.poll() is None, "catalog writer did not wait for shared lock")
            fcntl.flock(lock, fcntl.LOCK_UN)
        stdout, stderr = waiting.communicate(timeout=10)
        require(
            waiting.returncode == 0,
            f"serialized writer failed\nstdout: {stdout}\nstderr: {stderr}",
        )

        wrapper = make_wrapper(root, binary)
        environment = os.environ.copy()
        environment["MCPO_TEST_REAL_BINARY"] = str(binary)

        failed_runtime = root / "failed-runtime"
        failed_environment = environment | {"MCPO_TEST_MODE": "fail"}
        failed = run(
            maintenance_command(tool, wrapper, database, failed_runtime, base),
            env=failed_environment,
        )
        require(failed.returncode == 47, "staging failure exit was not preserved", failed)
        require(file_digest(database) == original_digest, "staging failure changed live database")
        failed_status = json.loads(
            (failed_runtime / "last-success.json").read_text(encoding="utf-8")
        )
        require(
            failed_status["state"] == "failed"
            and failed_status["failure"]["category"] == "collection_failure",
            "collection failure status is incomplete",
        )

        corrupt_runtime = root / "corrupt-runtime"
        corrupt = run(
            maintenance_command(tool, wrapper, database, corrupt_runtime, base),
            env=environment | {"MCPO_TEST_MODE": "corrupt"},
        )
        require(corrupt.returncode != 0, "corrupt staged database was published", corrupt)
        require(file_digest(database) == original_digest, "integrity failure changed live database")
        corrupt_status = json.loads(
            (corrupt_runtime / "last-success.json").read_text(encoding="utf-8")
        )
        require(
            corrupt_status["failure"]["category"] == "integrity_failure",
            "integrity failure status category is wrong",
        )

        interrupted_runtime = root / "interrupted-runtime"
        interrupted = subprocess.Popen(
            maintenance_command(tool, wrapper, database, interrupted_runtime, base),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=environment | {"MCPO_TEST_MODE": "sleep"},
            start_new_session=True,
        )
        time.sleep(0.5)
        os.killpg(interrupted.pid, signal.SIGTERM)
        interrupted.communicate(timeout=10)
        require(file_digest(database) == original_digest, "interruption changed live database")

        success_runtime = root / "success-runtime"
        observations = []
        stop_reader = threading.Event()

        def reader():
            while not stop_reader.is_set():
                try:
                    observations.append(snapshot_count(database))
                except sqlite3.Error as error:
                    observations.append(str(error))

        reader_thread = threading.Thread(target=reader)
        reader_thread.start()
        succeeded = run(
            maintenance_command(tool, binary, database, success_runtime, base)
        )
        stop_reader.set()
        reader_thread.join(timeout=5)
        require(succeeded.returncode == 0, "staged refresh failed", succeeded)
        require(snapshot_count(database) == 2, "refreshed snapshot was not published")
        require(
            observations and set(observations).issubset({1, 2}),
            f"readers observed non-atomic database state: {set(observations)}",
        )
        status = json.loads(
            (success_runtime / "last-success.json").read_text(encoding="utf-8")
        )
        require(
            status["state"] == "succeeded"
            and len(status["last_success"]["snapshot_digest"]) == 64
            and status["last_success"]["counts"]["catalog_snapshots"] == 2
            and status["failure"] is None,
            "last-success record is incomplete",
        )

        failed_after_success = run(
            maintenance_command(tool, wrapper, database, success_runtime, base),
            env=environment | {"MCPO_TEST_MODE": "fail"},
        )
        require(failed_after_success.returncode == 47, "later failure was not recorded")
        retained = json.loads(
            (success_runtime / "last-success.json").read_text(encoding="utf-8")
        )
        require(
            retained["state"] == "failed"
            and retained["last_success"]["snapshot_digest"]
            == status["last_success"]["snapshot_digest"],
            "failure status did not retain the last successful publication",
        )

        backup = root / "verified-backup.sqlite"
        backed_up = run(
            [
                sys.executable,
                tool,
                "backup",
                "--database",
                database,
                "--output",
                backup,
            ]
        )
        require(backed_up.returncode == 0 and Path(f"{backup}.json").is_file(), "backup failed", backed_up)

        with sqlite3.connect(database) as connection:
            connection.execute("DELETE FROM snapshot_server_versions")
            connection.execute("DELETE FROM snapshots")
            connection.commit()
        restored = run(
            [
                sys.executable,
                tool,
                "restore",
                "--database",
                database,
                "--backup",
                backup,
            ]
        )
        require(restored.returncode == 0, "verified restore failed", restored)
        require(snapshot_count(database) == 2, "restore did not recover snapshot history")

        before_rejected_restore = file_digest(database)
        with backup.open("r+b") as output:
            output.seek(100)
            output.write(b"tamper")
        rejected = run(
            [
                sys.executable,
                tool,
                "restore",
                "--database",
                database,
                "--backup",
                backup,
            ]
        )
        require(rejected.returncode != 0, "tampered backup was restored", rejected)
        require(
            file_digest(database) == before_rejected_restore,
            "rejected restore changed live database",
        )
    finally:
        server.shutdown()
        server.server_close()
        shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, OSError, sqlite3.Error, subprocess.SubprocessError) as error:
        print(f"maintenance test failure: {error}", file=sys.stderr)
        raise SystemExit(1)
