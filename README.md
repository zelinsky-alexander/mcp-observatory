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
- SQLite 3 development files for the local Registry explorer
- Python 3 for the offline loopback HTTP tests

On Ubuntu/WSL, SQLite development files are provided by `libsqlite3-dev`.

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

./build/dev-debug/mcp-observatory registry refresh \
  --database ./db/local-registry.sqlite \
  --output ./registry-refresh

./build/dev-debug/mcp-observatory bundle validate ./official-run
```

## Portable registry collection

Production collection uses the compiled Official MCP Registry default:

```bash
./build/dev-debug/mcp-observatory registry collect \
  --output ./official-run
```

For an unattended collection:

```bash
./build/release/mcp-observatory registry collect \
  --registry-base-url https://registry.modelcontextprotocol.io \
  --output official-run \
  --request-timeout-seconds 60 \
  --stall-timeout-seconds 300 \
  --run-timeout-seconds 0 \
  --maximum-attempts-per-page 8 \
  --retry-initial-seconds 2 \
  --retry-maximum-seconds 120 \
  --maximum-pages 5000 \
  --maximum-records 500000 \
  --verbose
```

For a bounded CI collection:

```bash
./build/release/mcp-observatory registry collect \
  --registry-base-url http://127.0.0.1:8080 \
  --output ./ci-run \
  --maximum-pages 100 \
  --maximum-page-bytes 8388608 \
  --maximum-records 10000 \
  --request-timeout-seconds 30 \
  --stall-timeout-seconds 90 \
  --run-timeout-seconds 600 \
  --maximum-attempts-per-page 3 \
  --retry-initial-seconds 1 \
  --retry-maximum-seconds 10
```

`--registry-base-url` overrides `MCPO_REGISTRY_BASE_URL`, which overrides
`https://registry.modelcontextprotocol.io`. `--maximum-redirects` is also
supported. Raw pages are mandatory evidence in bundle version 1;
`--retain-raw` is accepted explicitly for forward-compatible scripts.

`run_timeout_seconds=0` means unlimited total runtime.
`request_timeout_seconds` bounds one HTTP attempt.
`stall_timeout_seconds` detects lack of durable page completion. Heartbeats
prove process liveness only; they do not reset the stall watchdog. Durable
progress means a page, its page-history metadata, and the compact checkpoint
were atomically committed.

Transient request failures and HTTP 408, 425, 429, 500, 502, 503, and 504 use
bounded exponential retries. `Retry-After` supports integer seconds only in
this version and is capped at the configured retry maximum. Other header
formats fall back to exponential backoff.

Add `--verbose` for human-readable progress on standard error. Request-wait
heartbeats are distinct from retry-backoff heartbeats and durable page
completion. Standard output remains reserved for the concise command result.
Cursor values, paths, URLs, and diagnostics are escaped and bounded.

Example progress:

```text
[registry] mode=resume registry=https://registry.modelcontextprotocol.io output=official-run resume_source=legacy-partial
[registry] request_timeout=60s stall_timeout=300s run_timeout=unlimited maximum_attempts_per_page=8 retry_initial=2s retry_maximum=120s
[registry] completed_pages=280 completed_records=8400 next_page=281 next_cursor=yes cursor_prefix=abc... cursor_length=96
[registry] page=281 attempt=1/8 request_start elapsed=12.4s request_timeout=60.0s since_last_page=1.4s cursor_prefix=abc... cursor_length=96
[registry] heartbeat state=request_wait page=281 attempt=1/8 request_wait=5s since_last_page=6s completed_pages=280 completed_records=8400 elapsed=17s
[registry] page=281 attempt=1/8 retryable_http_status=503 retry_in=2s
[registry] heartbeat state=retry_backoff page=281 next_attempt=2/8 retry_remaining=0s since_last_page=8s completed_pages=280 completed_records=8400 elapsed=19s
[registry] page=281 status=200 bytes=24821 duration=8.1s records=30 total_records=8430 completed_pages=281 next_cursor=yes saved=raw/page-000281.json checkpoint=committed
```

To retain progress separately from machine-readable output:

```bash
mcp-observatory registry collect ... \
  --verbose 2>registry-progress.log
```

### Compact checkpoints and resume

After each completed page, `checkpoint.json` stores only the current durable
resume head. Raw page files and `raw/pages.jsonl` remain the authoritative page
history. The checkpoint records the completed counts, next cursor, last-page
artifact, page-metadata artifact, provenance, timestamp, and status. It does
not contain the growing historical artifact list.

Resume validates the checkpoint version and provenance, last-page and
page-metadata sizes and SHA-256 values, metadata sequence, cursor continuity,
and safe relative paths before network collection starts. It continues from
`completed_pages + 1`. A raw page fetched but not checkpointed is ignored and
downloaded again; committed pages are not downloaded again.

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

### Full collection, resume, and periodic refresh

`registry collect` starts a new full collection. `registry collect --resume`
continues the first uncommitted page of an interrupted cursor chain.
`registry refresh` instead starts a new immutable incremental collection and
applies the Official Registry `updated_since` filter to every cursor request.

Refresh requires a completed snapshot already indexed in SQLite. It opens the
catalog read-only first and selects the greatest bytewise `completed_at`, with
the highest local snapshot ID breaking ties. That exact timestamp is the
default filter, while the baseline snapshot SHA-256 and completion timestamp
are recorded as portable provenance. A missing baseline fails as
`no_baseline_snapshot` before network access, output creation, or database
modification; refresh never silently bootstraps a full collection.

```bash
./build/release/mcp-observatory registry refresh \
  --database db/local-registry.sqlite \
  --output registry-refresh-20260727T120000Z
```

For testing or recovery, override only the filter value:

```bash
./build/release/mcp-observatory registry refresh \
  --database db/local-registry.sqlite \
  --output registry-refresh-recovery \
  --updated-since 2026-07-26T18:42:11Z \
  --format json
```

The timestamp must be exactly `YYYY-MM-DDTHH:MM:SSZ`; it is percent-encoded in
requests and preserved byte-for-byte in checkpoint and bundle evidence.
Refresh accepts the collector's existing timeout, retry, redirect, page,
record, byte, resume, Registry URL, and verbose options. Incremental
checkpoints bind the Registry URL, collection mode, exact timestamp, baseline
digest, and baseline completion time. Changing any bound provenance fails
closed. Legacy checkpoints remain full-collection checkpoints.

A valid zero-record response succeeds, creates a validated immutable bundle,
and imports a new zero-link snapshot. Absence from an incremental response
never means deletion, inactivity, or removal; historical snapshots, records,
and links remain unchanged.

Text output uses stable `name=value` fields:

```text
status=completed
collection_mode=incremental
updated_since=2026-07-26T18:42:11Z
base_snapshot_sha256=<baseline-digest>
snapshot_sha256=<incremental-digest>
completed_pages=2
received_records=37
inserted_server_versions=12
reused_server_versions=25
changed_identity_records=4
snapshot_links_created=37
```

`--format json` emits the same fields as one JSON object with numeric counts.
Progress goes only to standard error.

If collection succeeds but transactional import fails, refresh preserves the
valid bundle and `_SUCCESS`, rolls back the database fully, exits non-zero,
and reports `collection_completed_import_failed`. After resolving the local
catalog issue, retry the existing importer:

```bash
mcp-observatory registry index \
  --bundle registry-refresh-recovery \
  --database db/local-registry.sqlite
```

Example scheduled workflow (the project does not install or enable a
scheduler):

```bash
stamp="$(date -u +%Y%m%dT%H%M%SZ)"

mcp-observatory registry refresh \
  --database db/local-registry.sqlite \
  --output "registry-refresh-${stamp}" \
  --request-timeout-seconds 60 \
  --stall-timeout-seconds 300 \
  --run-timeout-seconds 900 \
  --maximum-attempts-per-page 8 \
  --verbose
```

Current limitations: refresh requires an indexed baseline; it does not infer
removals, construct a merged current-state snapshot, download packages,
execute servers, invoke MCP tools, or manage scheduling or retention.

## Local Registry Explorer v0.1

The explorer indexes official registry metadata from an immutable completed
bundle. It performs no network enrichment. It does not determine whether an
MCP server is safe, vulnerable or malicious. Security findings and
longitudinal comparison are planned for later milestones.

SQLite is required through the system development package; SQLite source is
not vendored. Indexing first runs the authoritative full bundle validator,
then streams `canonical/servers.jsonl` into one `BEGIN IMMEDIATE` transaction.
A failed import is rolled back and exposes no snapshot. The source bundle is
never modified.

```bash
./build/release/mcp-observatory registry index \
  --bundle /home/alex/source/mcp-observatory/official-run-resumed \
  --database /home/alex/source/mcp-observatory/local-registry.sqlite

./build/release/mcp-observatory registry summarize \
  /home/alex/source/mcp-observatory/local-registry.sqlite

./build/release/mcp-observatory registry search \
  /home/alex/source/mcp-observatory/local-registry.sqlite github \
  --limit 20

./build/release/mcp-observatory registry list \
  /home/alex/source/mcp-observatory/local-registry.sqlite \
  --has-remote \
  --without-repository \
  --limit 50

./build/release/mcp-observatory registry show \
  /home/alex/source/mcp-observatory/local-registry.sqlite \
  io.example/server
```

`registry index` accepts `--maximum-records`,
`--maximum-line-bytes`, `--maximum-database-bytes`, and `--verbose`. Defaults
are 500,000 records, 8 MiB per canonical line, and 4 GiB for the database.

`registry summarize` selects the snapshot with the latest bytewise
`completed_at` value and highest snapshot ID tie-breaker unless
`--snapshot DIGEST` is supplied. It supports `--format text|json`.

`registry search` searches server identifiers, descriptions, package
identifiers, repository URLs, remote URLs, and remote hosts. It supports
snapshot selection, `--limit` (maximum 500), `--offset`, status/transport and
presence filters, and `--format text|jsonl`. Schema creation explicitly tries
FTS5. When the linked SQLite does not provide FTS5, the schema records `like`
mode and search uses prepared, escaped, bounded `LIKE` expressions with the
same filters and result shape.

`registry list` supports status, transport, package-registry, repository-host,
remote-host, and presence filters with `--limit` (maximum 1,000), `--offset`,
and text or JSONL output. `registry show` performs exact server-name matching,
optionally selects `--version`, and emits canonical JSON only with
`--include-canonical`. Version strings use deterministic bytewise ordering,
not semantic-version ordering.

The complete schema and import/query behavior are documented in
[`docs/registry-explorer-v1.md`](docs/registry-explorer-v1.md).

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
