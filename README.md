# MCP Observatory

MCP Observatory is a longitudinal security research platform for collecting, validating, storing, and comparing security-relevant observations of Model Context Protocol servers over time.

The project is a dependency-minimal C++20 single binary. It consumes versioned observation documents and can produce a bounded, deterministic filesystem bundle of public Official MCP Registry metadata.

> **Status:** early research prototype. Network activity is limited to the explicit registry collector. It performs no package installation, authentication, MCP server execution, or MCP tool invocation.

## Why it exists

An MCP server can keep a familiar tool name while changing its description, input schema, annotations, or surrounding tool inventory. Name-only approval cannot detect that drift. MCP Observatory provides the longitudinal analysis side of the planned relationship with `mcp-native-guard`, which remains the local sensor and enforcement boundary.

## Build

Requirements:

- CMake 3.24 or newer
- Ninja
- A C++20 compiler such as Clang 17+ or GCC 13+
- `/usr/bin/curl` and `/usr/bin/openssl` for registry collection and hashing
- Python 3 for the offline loopback HTTP tests

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

./build/dev-debug/mcp-observatory validate-observation \
  examples/observation-filesystem.json

./build/dev-debug/mcp-observatory ingest-observation \
  examples/observation-filesystem.json observations.jsonl

./build/dev-debug/mcp-observatory history summarize examples/history.jsonl

./build/dev-debug/mcp-observatory history latest \
  examples/history.jsonl local:filesystem:2026.7.10

./build/dev-debug/mcp-observatory history diff-latest \
  examples/history.jsonl local:filesystem:2026.7.10

./build/dev-debug/mcp-observatory registry collect \
  --output ./official-run

./build/dev-debug/mcp-observatory bundle validate ./official-run
```

## Portable registry collection

Production collection uses the compiled Official MCP Registry default:

```bash
./build/dev-debug/mcp-observatory registry collect \
  --output ./official-run
```

For a local fixture or compatible registry, including one mounted into a
Docker or Lambda container:

```bash
./build/dev-debug/mcp-observatory registry collect \
  --registry-base-url http://127.0.0.1:8080 \
  --output ./test-run \
  --maximum-pages 100 \
  --maximum-page-bytes 8388608 \
  --maximum-records 10000 \
  --request-timeout-seconds 30 \
  --run-timeout-seconds 600
```

`--registry-base-url` overrides `MCPO_REGISTRY_BASE_URL`, which overrides
`https://registry.modelcontextprotocol.io`. `--maximum-redirects` is also
supported. Raw pages are mandatory evidence in bundle version 1;
`--retain-raw` is accepted explicitly for forward-compatible scripts.

### Legacy partial checkpoints and resume

A legacy interrupted directory containing only `raw/page-*.json` can be
reconstructed without contacting the Registry:

```bash
./build/dev-debug/mcp-observatory registry checkpoint reconstruct \
  ./legacy-partial \
  --registry-base-url http://127.0.0.1:8080
```

This verifies strict contiguous numbering, response shape, cursor chaining,
configured limits, page size, and SHA-256; then atomically creates
`checkpoint.json` and rebuilds `raw/pages.jsonl`. It never creates `_SUCCESS`.

Continue into a new immutable output bundle with:

```bash
./build/dev-debug/mcp-observatory registry collect \
  --registry-base-url http://127.0.0.1:8080 \
  --resume ./legacy-partial \
  --output ./completed-run
```

The resume directory remains partial evidence and is not promoted or
overwritten. When the final reconstructed page has no next cursor, resume
performs no HTTP request and finalizes the new bundle from the validated raw
pages.

The same build commands work in Ubuntu and WSL. In Docker, CI, or an AWS Lambda
Linux container, install the build toolchain for compilation and ensure the
runtime image provides curl and OpenSSL at `/usr/bin`. Mount or select a
writable parent for `--output`; the destination itself must not exist.

Offline tests use only a loopback fixture server:

```bash
ctest --preset dev-debug
```

The live Registry check is opt-in, disabled from CTest, and requires network
access and a new destination:

```bash
tests/live_registry.sh \
  ./build/dev-debug/mcp-observatory \
  /tmp/mcpo-live-registry
```

Example systemd user unit files are in [`examples/systemd`](examples/systemd).
They are examples only and are never installed or enabled automatically.
Because bundle destinations are immutable, production scheduling should use a
fresh output path per run; safe path allocation belongs to the future
publisher/retention milestone.

## History analysis

`history summarize` validates every JSONL record and reports the total number of observations, unique targets, and the earliest and latest timestamps.

```text
records=3
targets=2
earliest_observed_at=2026-07-24T20:15:30Z
latest_observed_at=2026-07-25T20:15:30Z
```

`history latest` emits the complete compact observation with the greatest `observed_at` value for one target.

`history diff-latest` selects the two most recent observations for one target even when history records are not stored chronologically. It then compares their embedded inventories and reports deterministic drift output:

```text
target_id=local:filesystem:2026.7.10
before_observed_at=2026-07-24T20:15:30Z
after_observed_at=2026-07-25T20:15:30Z
verdict=material_drift
executable_changed=false
added=1
removed=1
modified=1
+ execute_command
- search
~ read_file
```

History analysis is bounded by maximum line size, record count, unique target count, observation size, nesting depth, inventory size, and tool limits. A malformed line invalidates the history instead of being skipped silently.

Exit codes for target history commands:

- `0`: success or the latest two inventories are identical
- `1`: invalid command-line use
- `2`: output or file-opening failure
- `3`: invalid history
- `4`: material drift detected by `diff-latest`
- `5`: target not found or fewer than two observations exist

## Observation version 1

```json
{
  "observation_version": 1,
  "observed_at": "2026-07-24T20:15:30Z",
  "target_id": "local:filesystem:2026.7.10",
  "source_type": "controlled_corpus",
  "sensor": {
    "name": "mcp-native-guard",
    "version": "0.1.0"
  },
  "configuration_profile": "default-no-network",
  "inventory": {
    "inventory_version": 1,
    "server": { "downstream_executable": "npx" },
    "tools": []
  }
}
```

Required identity fields reject escapes and control characters in version 1. `observed_at` must use the fixed UTC form `YYYY-MM-DDTHH:MM:SSZ`. The parser enforces bounded document size, identity length, nesting depth, inventory size, tool count, and tool-name size. Duplicate security-relevant fields and invalid embedded inventories fail closed.

The history store remains an append-only JSONL file intended for one local writer. It does not yet provide locking between concurrent writers, duplicate suppression, indexing, transactional recovery, retention, or database migration.

## Inventory drift comparison

The inventory reader accepts deterministic version-1 output from `mcp-native-guard inspect`. Tools must be sorted and unique. Each complete canonical tool object is compared byte-for-byte, so this milestone relies on the producer to canonicalize equivalent JSON definitions.

## Target manifests

Reviewed version-1 JSONL target manifests describe future collection candidates with explicit provenance. They do not trigger installation, execution, or network access.

## Project boundary

`mcp-native-guard` remains the bounded local MCP sensor and enforcement boundary. MCP Observatory consumes versioned observations and performs historical comparison. No shared library or shared object is introduced yet; stable file formats are the integration boundary.

See [`docs/research-boundaries.md`](docs/research-boundaries.md) for the safety posture.
See [`docs/registry-bundle-v1.md`](docs/registry-bundle-v1.md) for the bundle,
identity, hashing, URL, redirect, privacy, provenance, and validation rules.

## Licensing

Apache License 2.0. The implementation is original project code based on the stated requirements, C++ language rules, CMake documentation, and the documented observation and inventory contracts. No third-party source code is included.

Before publication or commercial use, perform manual licence, similarity, security, privacy, and legal review.
