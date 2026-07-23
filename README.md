# MCP Observatory

MCP Observatory is a longitudinal security research platform for collecting, normalizing, and comparing security-relevant observations of Model Context Protocol servers over time.

The project is designed as a dependency-free C++20 single binary. Its first milestone accepts reviewed target manifests and produces bounded, deterministic summaries. It does not install packages, execute third-party MCP servers, scan arbitrary Internet addresses, authenticate to remote services, or invoke MCP tools.

> **Status:** initial research scaffold. It is not a production scanner or security product.

## Initial scope

- Versioned JSONL target manifests with explicit provenance.
- Bounded input processing and deterministic output.
- Separation between target selection, future inspection workers, and observation analysis.
- No automatic execution of registry packages.
- No public endpoint index.
- No LLM in collection or security-decision paths.

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

## Current commands

```bash
./build/dev-debug/mcp-observatory about
./build/dev-debug/mcp-observatory validate-targets examples/targets.jsonl
./build/dev-debug/mcp-observatory summarize-targets examples/targets.jsonl
```

The target reader currently validates a deliberately small version-1 JSONL envelope. It is not a general JSON implementation. Unknown fields are accepted, while duplicate or malformed security-relevant fields are rejected.

## Planned relationship with `mcp-native-guard`

`mcp-native-guard` remains the local bounded MCP sensor and enforcement boundary. MCP Observatory will consume versioned, privacy-minimized observations produced by approved sensors and maintain longitudinal history. No shared library is introduced at this stage; stable file formats are the integration boundary.

## Safety posture

See [`docs/research-boundaries.md`](docs/research-boundaries.md). The initial code performs no network activity and launches no external process.

## Licensing

Apache License 2.0. The initial implementation is original project code based on the stated requirements, C++ language rules, CMake documentation, and the JSON data model described in this repository. No third-party source code is included.

Before publication or commercial use, perform manual licence, similarity, security, privacy, and legal review.
