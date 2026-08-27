from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import sqlite3
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

MODULE_PATH = Path(__file__).resolve().parents[1] / "tools" / "remote_runtime_discovery.py"
SPEC = importlib.util.spec_from_file_location("remote_runtime_discovery", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
remote = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(remote)


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        request = json.loads(self.rfile.read(length))
        method = request.get("method")
        if method == "notifications/initialized":
            self.send_response(202)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if method == "initialize":
            response = {
                "jsonrpc": "2.0",
                "id": request["id"],
                "result": {
                    "protocolVersion": "2025-06-18",
                    "capabilities": {"tools": {}},
                    "serverInfo": {"name": "fixture", "version": "1"},
                },
            }
            session = "fixture-session"
        else:
            response = {
                "jsonrpc": "2.0",
                "id": request["id"],
                "result": {
                    "tools": [
                        {"name": "zeta", "description": "last", "inputSchema": {"type": "object"}},
                        {"name": "alpha", "description": "first", "inputSchema": {"type": "object"}},
                    ]
                },
            }
            session = None
        payload = json.dumps(response, separators=(",", ":")).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        if session:
            self.send_header("Mcp-Session-Id", session)
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, *_args):
        return


def test_inspect_canonicalizes_declared_remote(monkeypatch):
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    monkeypatch.setattr(remote, "_public_addresses", lambda _host, _port: ["203.0.113.1"])
    try:
        result = remote.inspect(f"http://127.0.0.1:{server.server_port}/mcp", 3)
    finally:
        server.shutdown()
        thread.join(timeout=3)
    assert [tool["name"] for tool in result["inventory"]["tools"]] == ["alpha", "zeta"]
    assert result["inventory"]["transport"] == "streamable-http"
    assert result["session_id_sha256"] == remote.digest_text("fixture-session")


def test_private_destination_is_rejected():
    try:
        remote.validate_url("http://127.0.0.1/mcp")
    except ValueError as exc:
        assert "non-public" in str(exc)
    else:
        raise AssertionError("loopback destination was accepted")


def test_observation_persists_tools(tmp_path, monkeypatch):
    database = tmp_path / "catalog.sqlite"
    db = sqlite3.connect(database)
    db.executescript(
        """
        PRAGMA foreign_keys=ON;
        CREATE TABLE server_versions(id INTEGER PRIMARY KEY,server_identifier TEXT,server_version TEXT);
        CREATE TABLE remotes(id INTEGER PRIMARY KEY,server_version_id INTEGER REFERENCES server_versions(id),position INTEGER,url TEXT,scheme TEXT,host TEXT,port INTEGER,transport TEXT);
        INSERT INTO server_versions VALUES(1,'fixture/server','1.0.0');
        INSERT INTO remotes VALUES(10,1,0,'https://example.test/mcp','https','example.test',443,'streamable-http');
        """
    )
    db.close()
    monkeypatch.setattr(
        remote,
        "inspect",
        lambda _url, _timeout: {
            "inventory": {
                "schema_version": 1,
                "transport": "streamable-http",
                "protocol_version": "2025-06-18",
                "server_info": {"name": "fixture"},
                "capabilities": {"tools": {}},
                "tools": [{"name": "hello", "inputSchema": {"type": "object"}}],
            },
            "http_status": 200,
            "session_id_sha256": None,
        },
    )
    result = remote.observe(str(database), 10, 3, "a" * 64)
    assert result["status"] == "completed"
    db = sqlite3.connect(database)
    assert db.execute("SELECT COUNT(*) FROM runtime_remote_observation_tools").fetchone()[0] == 1
    db.close()
