# MCP Observatory

MCP Observatory is a longitudinal security research platform for collecting, normalizing, and comparing security-relevant observations of Model Context Protocol servers over time.

The project is a dependency-free C++20 single binary. The current milestone consumes deterministic version-1 inventories produced by `mcp-native-guard inspect` and detects material changes in server executable identity, tool presence, and canonical tool definitions.

> **Status:** early research prototype. It performs no network activity, package installation, authentication, third-party process execution, or MCP tool invocation.

## Why it exists

An MCP server can keep a familiar tool name while changing its description, input schema, annotations, or surrounding tool inventory. Name-only approval cannot detect that drift. MCP Observatory creates the longitudinal analysis side of the planned relationship with `mcp-native-guard`, which remains the local sensor and enforcement boundary.

## Build

Requirements:

- CMake 3.24 or newer
- Ninja
- A C++20 compiler such as Clang 17+ or GCC 13+

```bash
cmake --preset dev-debug
cmake --build --preset dev-debug
ctest --preset dev-debug
```

## Commands

```bash
./build/dev-debug/mcp-observatory about
./build/dev-debug/mcp-observatory validate-targets examples/targets.jsonl
./build/dev-debug/mcp-observatory summarize-targets examples/targets.jsonl
./build/dev-debug/mcp-observatory compare-inventories \
  examples/inventory-before.json examples/inventory-after.json
```

A changed inventory produces deterministic output such as:

```text
verdict=material_drift
executable_changed=false
added=1
removed=1
modified=1
+ execute_command
- search
~ read_file
```

Exit codes for `compare-inventories`:

- `0`: inventories are identical
- `1`: invalid command-line use
- `3`: an input inventory is invalid or cannot be opened
- `4`: material drift was detected

## Accepted inventory contract

The reader accepts the deterministic version-1 inventory shape documented by `mcp-native-guard inspect`:

```json
{
  "inventory_version": 1,
  "server": { "downstream_executable": "node" },
  "tools": [
    {
      "name": "read_file",
      "description": "Read one file",
      "inputSchema": {}
    }
  ]
}
```

The reader is deliberately bounded and narrow:

- maximum input bytes, tool count, nesting depth, and tool-name size;
- required version, server, executable, and tools fields;
- duplicate security-relevant fields are rejected;
- escaped identity strings are rejected in version 1;
- tools must be sorted and unique, matching the deterministic producer contract;
- each complete canonical tool object is compared byte-for-byte after parsing.

The current milestone relies on `mcp-native-guard` to canonicalize tool definitions. It does not yet independently canonicalize arbitrary equivalent JSON, calculate cryptographic fingerprints, persist history, or classify individual fields inside a modified definition.

## Target manifests

The earlier scaffold also supports reviewed version-1 JSONL target manifests with explicit provenance. Target manifests describe future collection candidates; they do not trigger installation, execution, or network access.

## Project boundary

`mcp-native-guard` remains the bounded local MCP sensor and enforcement boundary. MCP Observatory consumes versioned observations and performs historical comparison. No shared library or shared object is introduced yet; stable file formats are the integration boundary.

See [`docs/research-boundaries.md`](docs/research-boundaries.md) for the safety posture.

## Licensing

Apache License 2.0. The implementation is original project code based on the stated requirements, C++ language rules, CMake documentation, and the documented inventory data contract. No third-party source code is included.

Before publication or commercial use, perform manual licence, similarity, security, privacy, and legal review.
