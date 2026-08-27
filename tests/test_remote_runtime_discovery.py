#!/usr/bin/env python3
from __future__ import annotations

import importlib.util
from pathlib import Path
import sqlite3
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
SPEC = importlib.util.spec_from_file_location(
    "remote_runtime_discovery", ROOT / "tools" / "remote_runtime_discovery.py"
)
assert SPEC is not None and SPEC.loader is not None
remote = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(remote)


class FakeSocket:
    def close(self) -> None:
        pass


class RemoteRuntimeDiscoveryTests(unittest.TestCase):
    def test_private_destination_is_rejected(self) -> None:
        answer = [(2, 1, 6, "", ("127.0.0.1", 443))]
        with mock.patch.object(remote.socket, "getaddrinfo", return_value=answer):
            with self.assertRaisesRegex(remote.DestinationPolicyError, "non-public"):
                remote.validate_url("https://example.test/mcp")

    def test_http_socket_connects_to_prevalidated_address(self) -> None:
        connection = remote._PinnedHTTPConnection(
            "registry.example", 80, "203.0.113.44", 3
        )
        sock = FakeSocket()
        with mock.patch.object(
            remote.socket, "create_connection", return_value=sock
        ) as create:
            connection.connect()
        self.assertIs(connection.sock, sock)
        self.assertEqual(create.call_args.args[0], ("203.0.113.44", 80))

    def test_https_keeps_hostname_for_tls_sni(self) -> None:
        connection = remote._PinnedHTTPSConnection(
            "registry.example", 443, "203.0.113.45", 3
        )
        raw = FakeSocket()
        wrapped = FakeSocket()
        context = mock.Mock()
        context.wrap_socket.return_value = wrapped
        connection._context = context
        with mock.patch.object(
            remote.socket, "create_connection", return_value=raw
        ) as create:
            connection.connect()
        self.assertEqual(create.call_args.args[0], ("203.0.113.45", 443))
        context.wrap_socket.assert_called_once_with(
            raw, server_hostname="registry.example"
        )
        self.assertIs(connection.sock, wrapped)

    def test_tools_list_pagination_produces_complete_sorted_inventory(self) -> None:
        responses = [
            (
                {
                    "jsonrpc": "2.0",
                    "id": 1,
                    "result": {
                        "protocolVersion": "2025-06-18",
                        "capabilities": {"tools": {}},
                        "serverInfo": {"name": "fixture", "version": "1"},
                    },
                },
                {"mcp-session-id": "session"},
                200,
                100,
            ),
            (None, {}, 202, 0),
            (
                {
                    "jsonrpc": "2.0",
                    "id": 2,
                    "result": {
                        "tools": [{"name": "zeta", "inputSchema": {"type": "object"}}],
                        "nextCursor": "page-2",
                    },
                },
                {},
                200,
                100,
            ),
            (
                {
                    "jsonrpc": "2.0",
                    "id": 3,
                    "result": {
                        "tools": [{"name": "alpha", "inputSchema": {"type": "object"}}]
                    },
                },
                {},
                200,
                100,
            ),
        ]
        with mock.patch.object(
            remote, "validate_url", return_value=("https", "example.test", 443, "/mcp")
        ), mock.patch.object(remote, "_request", side_effect=responses) as request:
            result = remote.inspect("https://example.test/mcp", 3)
        self.assertEqual(
            [tool["name"] for tool in result["inventory"]["tools"]],
            ["alpha", "zeta"],
        )
        calls = request.call_args_list
        self.assertEqual(calls[2].args[4]["params"], {})
        self.assertEqual(calls[3].args[4]["params"], {"cursor": "page-2"})

    def test_repeated_pagination_cursor_is_protocol_invalid(self) -> None:
        responses = [
            (
                {
                    "jsonrpc": "2.0",
                    "id": 1,
                    "result": {"protocolVersion": "2025-06-18"},
                },
                {},
                200,
                10,
            ),
            (None, {}, 202, 0),
            (
                {
                    "jsonrpc": "2.0",
                    "id": 2,
                    "result": {"tools": [], "nextCursor": "same"},
                },
                {},
                200,
                10,
            ),
            (
                {
                    "jsonrpc": "2.0",
                    "id": 3,
                    "result": {"tools": [], "nextCursor": "same"},
                },
                {},
                200,
                10,
            ),
        ]
        with mock.patch.object(
            remote, "validate_url", return_value=("https", "example.test", 443, "/mcp")
        ), mock.patch.object(remote, "_request", side_effect=responses):
            with self.assertRaisesRegex(ValueError, "repeated pagination cursor"):
                remote.inspect("https://example.test/mcp", 3)

    def test_observation_limit_is_inconclusive(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            database = Path(temporary) / "catalog.sqlite"
            db = sqlite3.connect(database)
            db.executescript(
                """
                PRAGMA foreign_keys=ON;
                CREATE TABLE server_versions(
                  id INTEGER PRIMARY KEY,server_identifier TEXT,server_version TEXT
                );
                CREATE TABLE remotes(
                  id INTEGER PRIMARY KEY,server_version_id INTEGER REFERENCES server_versions(id),
                  position INTEGER,url TEXT,scheme TEXT,host TEXT,port INTEGER,transport TEXT
                );
                INSERT INTO server_versions VALUES(1,'fixture/server','1.0.0');
                INSERT INTO remotes VALUES(
                  10,1,0,'https://example.test/mcp','https','example.test',443,'streamable-http'
                );
                """
            )
            db.close()
            with mock.patch.object(
                remote,
                "inspect",
                side_effect=remote.ObservationLimitError("too many tool pages"),
            ):
                result = remote.observe(str(database), 10, 3, "a" * 64)
            self.assertEqual(result["status"], "inconclusive")
            self.assertEqual(result["error_stage"], "observation_limit")

    def test_observation_persists_tools(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            database = Path(temporary) / "catalog.sqlite"
            db = sqlite3.connect(database)
            db.executescript(
                """
                PRAGMA foreign_keys=ON;
                CREATE TABLE server_versions(
                  id INTEGER PRIMARY KEY,server_identifier TEXT,server_version TEXT
                );
                CREATE TABLE remotes(
                  id INTEGER PRIMARY KEY,server_version_id INTEGER REFERENCES server_versions(id),
                  position INTEGER,url TEXT,scheme TEXT,host TEXT,port INTEGER,transport TEXT
                );
                INSERT INTO server_versions VALUES(1,'fixture/server','1.0.0');
                INSERT INTO remotes VALUES(
                  10,1,0,'https://example.test/mcp','https','example.test',443,'streamable-http'
                );
                """
            )
            db.close()
            observation = {
                "inventory": {
                    "schema_version": 1,
                    "transport": "streamable-http",
                    "protocol_version": "2025-06-18",
                    "server_info": {"name": "fixture"},
                    "capabilities": {"tools": {}},
                    "tools": [
                        {"name": "hello", "inputSchema": {"type": "object"}}
                    ],
                },
                "http_status": 200,
                "session_id_sha256": None,
            }
            with mock.patch.object(remote, "inspect", return_value=observation):
                result = remote.observe(str(database), 10, 3, "a" * 64)
            self.assertEqual(result["status"], "completed")
            db = sqlite3.connect(database)
            self.assertEqual(
                db.execute(
                    "SELECT COUNT(*) FROM runtime_remote_observation_tools"
                ).fetchone()[0],
                1,
            )
            db.close()


if __name__ == "__main__":
    unittest.main()
