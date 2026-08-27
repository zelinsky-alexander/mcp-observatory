#!/usr/bin/env python3
"""Bounded MCP discovery for exact remote URLs declared by the registry.

This runner is intentionally narrow: it probes only an already-declared HTTP(S)
remote URL, performs initialize -> notifications/initialized -> tools/list, never
invokes tools, never follows redirects, and rejects loopback/private/link-local/
reserved destinations after DNS resolution. Results are persisted as an
observation-first record that can be mirrored into the public hot catalog.

No third-party Python dependency is used; the implementation relies only on the
Python standard library and the MCP Streamable HTTP request/response shape.
"""

from __future__ import annotations

import argparse
import hashlib
import http.client
import ipaddress
import json
import socket
import sqlite3
import ssl
import time
from typing import Any
from urllib.parse import urlsplit

SCHEMA = """
CREATE TABLE IF NOT EXISTS runtime_remote_observation_runs(
  id INTEGER PRIMARY KEY,
  server_version_id INTEGER NOT NULL REFERENCES server_versions(id) ON DELETE RESTRICT,
  remote_id INTEGER NOT NULL REFERENCES remotes(id) ON DELETE RESTRICT,
  status TEXT NOT NULL CHECK(status IN ('running','completed','failed','blocked','inconclusive')),
  declared_url TEXT NOT NULL,
  transport TEXT NOT NULL,
  probe_profile_sha256 TEXT NOT NULL,
  inventory_sha256 TEXT,
  inventory_json TEXT,
  protocol_version TEXT,
  server_info_json TEXT,
  http_status INTEGER,
  session_id_sha256 TEXT,
  started_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
  completed_at TEXT,
  error_stage TEXT,
  error_message TEXT
);
CREATE INDEX IF NOT EXISTS runtime_remote_observation_lookup
ON runtime_remote_observation_runs(remote_id,status,id);
CREATE TABLE IF NOT EXISTS runtime_remote_observation_tools(
  run_id INTEGER NOT NULL REFERENCES runtime_remote_observation_runs(id) ON DELETE CASCADE,
  name TEXT NOT NULL,
  definition_json TEXT NOT NULL,
  definition_sha256 TEXT NOT NULL,
  PRIMARY KEY(run_id,name)
);
"""

MAX_BODY_BYTES = 1_048_576
DEFAULT_PROTOCOL_VERSION = "2025-06-18"
USER_AGENT = "Open-MCP-Longitudinal-Assurance/remote-discovery-v1"


def canonical(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"))


def digest_text(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def connect(path: str) -> sqlite3.Connection:
    db = sqlite3.connect(path, timeout=30)
    db.row_factory = sqlite3.Row
    db.execute("PRAGMA foreign_keys=ON")
    db.execute("PRAGMA busy_timeout=30000")
    db.executescript(SCHEMA)
    return db


def _public_addresses(host: str, port: int) -> list[str]:
    addresses: list[str] = []
    for family, _, _, _, sockaddr in socket.getaddrinfo(host, port, type=socket.SOCK_STREAM):
        if family not in (socket.AF_INET, socket.AF_INET6):
            continue
        value = str(sockaddr[0]).split("%", 1)[0]
        address = ipaddress.ip_address(value)
        if not address.is_global:
            raise ValueError(f"declared remote resolves to non-public address: {address}")
        addresses.append(str(address))
    if not addresses:
        raise ValueError("declared remote hostname did not resolve to a public IP address")
    return sorted(set(addresses))


def validate_url(value: str) -> tuple[str, str, int, str]:
    parsed = urlsplit(value)
    if parsed.scheme not in {"https", "http"}:
        raise ValueError("declared remote URL must use http or https")
    if not parsed.hostname or parsed.username is not None or parsed.password is not None:
        raise ValueError("declared remote URL must have a hostname and no embedded credentials")
    if parsed.fragment:
        raise ValueError("declared remote URL must not contain a fragment")
    port = parsed.port or (443 if parsed.scheme == "https" else 80)
    _public_addresses(parsed.hostname, port)
    path = parsed.path or "/"
    if parsed.query:
        path += "?" + parsed.query
    return parsed.scheme, parsed.hostname, port, path


def _decode_response(content_type: str, body: bytes, request_id: int) -> dict[str, Any]:
    text = body.decode("utf-8", "strict")
    media = content_type.split(";", 1)[0].strip().lower()
    if media == "text/event-stream":
        payloads: list[dict[str, Any]] = []
        data_lines: list[str] = []
        for line in text.splitlines() + [""]:
            if line == "":
                if data_lines:
                    value = json.loads("\n".join(data_lines))
                    if isinstance(value, dict):
                        payloads.append(value)
                    data_lines.clear()
                continue
            if line.startswith("data:"):
                data_lines.append(line[5:].lstrip())
        for value in payloads:
            if value.get("id") == request_id:
                return value
        raise ValueError("SSE response contained no matching JSON-RPC response")
    value = json.loads(text)
    if not isinstance(value, dict):
        raise ValueError("remote response is not a JSON object")
    return value


def _request(
    scheme: str,
    host: str,
    port: int,
    path: str,
    message: dict[str, Any],
    *,
    timeout: int,
    session_id: str | None,
    protocol_version: str | None,
    expect_response: bool,
) -> tuple[dict[str, Any] | None, dict[str, str], int]:
    # Resolve and validate immediately before each connection to reduce DNS-rebinding risk.
    _public_addresses(host, port)
    if scheme == "https":
        connection: http.client.HTTPConnection = http.client.HTTPSConnection(
            host, port, timeout=timeout, context=ssl.create_default_context()
        )
    else:
        connection = http.client.HTTPConnection(host, port, timeout=timeout)
    payload = canonical(message).encode("utf-8")
    headers = {
        "Content-Type": "application/json",
        "Accept": "application/json, text/event-stream",
        "User-Agent": USER_AGENT,
        "Connection": "close",
    }
    if session_id:
        headers["Mcp-Session-Id"] = session_id
    if protocol_version:
        headers["MCP-Protocol-Version"] = protocol_version
    try:
        connection.request("POST", path, body=payload, headers=headers)
        response = connection.getresponse()
        status = int(response.status)
        response_headers = {name.lower(): value for name, value in response.getheaders()}
        if 300 <= status < 400:
            raise ValueError("declared remote attempted HTTP redirect; redirects are not followed")
        body = response.read(MAX_BODY_BYTES + 1)
        if len(body) > MAX_BODY_BYTES:
            raise ValueError("remote response exceeded configured body limit")
        if status in (401, 403):
            raise PermissionError(f"remote endpoint requires authentication (HTTP {status})")
        if status < 200 or status >= 300:
            raise ConnectionError(f"remote endpoint returned HTTP {status}")
        if not expect_response or status == 202 or not body:
            return None, response_headers, status
        result = _decode_response(response_headers.get("content-type", ""), body, int(message["id"]))
        if result.get("jsonrpc") != "2.0" or result.get("id") != message["id"]:
            raise ValueError("remote returned an invalid JSON-RPC response envelope")
        if "error" in result:
            raise ValueError("remote returned JSON-RPC error: " + canonical(result["error"])[:512])
        if "result" not in result:
            raise ValueError("remote JSON-RPC response has no result")
        return result, response_headers, status
    finally:
        connection.close()


def inspect(url: str, timeout: int) -> dict[str, Any]:
    scheme, host, port, path = validate_url(url)
    initialize = {
        "jsonrpc": "2.0",
        "id": 1,
        "method": "initialize",
        "params": {
            "protocolVersion": DEFAULT_PROTOCOL_VERSION,
            "capabilities": {},
            "clientInfo": {"name": "mcp-assurance-remote-observer", "version": "1"},
        },
    }
    init_response, headers, status = _request(
        scheme, host, port, path, initialize,
        timeout=timeout, session_id=None, protocol_version=None, expect_response=True,
    )
    assert init_response is not None
    init_result = init_response.get("result")
    if not isinstance(init_result, dict):
        raise ValueError("initialize result is not an object")
    protocol_version = str(init_result.get("protocolVersion") or DEFAULT_PROTOCOL_VERSION)
    session_id = headers.get("mcp-session-id")

    initialized = {"jsonrpc": "2.0", "method": "notifications/initialized"}
    _request(
        scheme, host, port, path, initialized,
        timeout=timeout, session_id=session_id, protocol_version=protocol_version,
        expect_response=False,
    )
    tools_request = {"jsonrpc": "2.0", "id": 2, "method": "tools/list", "params": {}}
    tools_response, _, tools_status = _request(
        scheme, host, port, path, tools_request,
        timeout=timeout, session_id=session_id, protocol_version=protocol_version,
        expect_response=True,
    )
    assert tools_response is not None
    result = tools_response.get("result")
    if not isinstance(result, dict) or not isinstance(result.get("tools"), list):
        raise ValueError("tools/list result has no tools array")
    tools: list[dict[str, Any]] = []
    for tool in result["tools"]:
        if not isinstance(tool, dict) or not isinstance(tool.get("name"), str) or not tool["name"]:
            raise ValueError("tools/list contains an invalid tool definition")
        tools.append(tool)
    tools.sort(key=lambda item: item["name"])
    inventory = {
        "schema_version": 1,
        "transport": "streamable-http",
        "protocol_version": protocol_version,
        "server_info": init_result.get("serverInfo"),
        "capabilities": init_result.get("capabilities", {}),
        "tools": tools,
    }
    return {
        "inventory": inventory,
        "http_status": tools_status or status,
        "session_id_sha256": digest_text(session_id) if session_id else None,
    }


def observe(database: str, remote_id: int, timeout: int, profile_sha256: str) -> dict[str, Any]:
    db = connect(database)
    try:
        remote = db.execute(
            """SELECT r.id,r.server_version_id,r.url,r.transport,sv.server_identifier,sv.server_version
               FROM remotes r JOIN server_versions sv ON sv.id=r.server_version_id
               WHERE r.id=?""",
            (remote_id,),
        ).fetchone()
        if remote is None:
            raise ValueError("remote_id does not exist")
        declared_url = str(remote["url"])
        transport = str(remote["transport"] or "")
        with db:
            cursor = db.execute(
                """INSERT INTO runtime_remote_observation_runs(
                     server_version_id,remote_id,status,declared_url,transport,probe_profile_sha256)
                   VALUES(?,?,'running',?,?,?)""",
                (int(remote["server_version_id"]), remote_id, declared_url, transport, profile_sha256),
            )
            run_id = int(cursor.lastrowid)

        state = "failed"
        stage = "probe"
        message: str | None = None
        result: dict[str, Any] | None = None
        try:
            result = inspect(declared_url, timeout)
            state = "completed"
            stage = "completed"
        except PermissionError as exc:
            state, stage, message = "blocked", "authentication", str(exc)
        except (socket.timeout, TimeoutError) as exc:
            state, stage, message = "inconclusive", "timeout", str(exc)
        except (OSError, ConnectionError) as exc:
            state, stage, message = "inconclusive", "connection", str(exc)
        except (ValueError, json.JSONDecodeError, UnicodeError) as exc:
            state, stage, message = "failed", "protocol", str(exc)

        with db:
            if state == "completed" and result is not None:
                inventory = result["inventory"]
                inventory_json = canonical(inventory)
                inventory_sha256 = digest_text(inventory_json)
                db.execute(
                    """UPDATE runtime_remote_observation_runs
                       SET status='completed',inventory_sha256=?,inventory_json=?,protocol_version=?,
                           server_info_json=?,http_status=?,session_id_sha256=?,completed_at=CURRENT_TIMESTAMP,
                           error_stage=NULL,error_message=NULL WHERE id=?""",
                    (
                        inventory_sha256,
                        inventory_json,
                        inventory.get("protocol_version"),
                        canonical(inventory.get("server_info")) if inventory.get("server_info") is not None else None,
                        result.get("http_status"),
                        result.get("session_id_sha256"),
                        run_id,
                    ),
                )
                for tool in inventory["tools"]:
                    definition = canonical(tool)
                    db.execute(
                        """INSERT INTO runtime_remote_observation_tools(
                             run_id,name,definition_json,definition_sha256) VALUES(?,?,?,?)""",
                        (run_id, tool["name"], definition, digest_text(definition)),
                    )
            else:
                db.execute(
                    """UPDATE runtime_remote_observation_runs
                       SET status=?,completed_at=CURRENT_TIMESTAMP,error_stage=?,error_message=? WHERE id=?""",
                    (state, stage, (message or "")[:2048], run_id),
                )
        return {"run_id": run_id, "status": state, "error_stage": None if state == "completed" else stage}
    finally:
        db.close()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--database", required=True)
    parser.add_argument("--remote-id", required=True, type=int)
    parser.add_argument("--timeout", type=int, default=15)
    parser.add_argument("--probe-profile-sha256", required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.timeout < 1 or args.timeout > 120:
        raise ValueError("timeout must be between 1 and 120 seconds")
    if len(args.probe_profile_sha256) != 64:
        raise ValueError("probe profile digest must be SHA-256 hex")
    result = observe(args.database, args.remote_id, args.timeout, args.probe_profile_sha256)
    print(canonical(result))
    return 0 if result["status"] == "completed" else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, sqlite3.Error, ValueError) as exc:
        print(f"remote runtime discovery failed: {exc}", file=__import__("sys").stderr)
        raise SystemExit(2)
