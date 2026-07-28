# Initial architecture

MCP Observatory begins as a bounded local collector and analysis tool, not as an Internet-wide scanner.

```text
reviewed target manifest
        |
        v
bounded manifest reader
        |
        v
future isolated inspection worker
        |
        v
versioned observation records
        |
        v
longitudinal comparison and reporting
```

## Single binary

The `mcp-observatory` executable owns command dispatch and links a small C++20 core library only for build organization and tests. The delivered program remains one binary.

Most commands perform no network activity. Reviewed network exceptions are
`registry collect`, `registry refresh`, and the download stage of
`analyze package`. Static extraction and scanning for package analysis run in a
disposable Docker container with network disabled.

## Integration boundary with mcp-native-guard

The projects remain separate. `mcp-native-guard` may later emit versioned observation documents containing deterministic server, capability, tool-definition, and policy fingerprints. Observatory will validate and ingest those documents.

A stable serialized schema is preferred over a shared object or copied implementation until both projects expose a proven reusable boundary.

## Planned stages

1. Reviewed target-manifest ingestion.
2. Versioned observation ingestion from controlled `mcp-native-guard inspect` runs.
3. Deterministic comparison and material-drift classification.
4. Isolated local-package workers.
5. Bounded Streamable HTTP inspection for explicitly approved remote targets.
6. Persistent history and aggregate reports.

Distributed queues, multiple databases, Internet-wide address scanning, and a public endpoint index are intentionally out of scope until evidence justifies them.

## Registry bundle boundary

The portable registry collector produces a version-1 filesystem bundle before
any storage decision:

```text
configured public registry
        |
        v
bounded same-origin HTTP collector
        |
        v
raw pages + sorted canonical JSONL + hashes
        |
        v
bundle validate
        |
        v
transactional local SQLite explorer
```

Collection has no SQLite or AWS dependency. The local explorer consumes only a
successful, fully validated bundle, keeps official metadata in normalized
schema-version-1 tables, and never modifies the evidence bundle. Explorer
commands perform no network activity. Future derived analysis remains
separate from the official imported metadata tables.

The collector has no fixed total deadline by default. Each HTTP attempt is
bounded, while a separate durable-progress watchdog detects a lack of committed
pages. Transient transport and selected HTTP failures use bounded retries.
After every page, the raw page, authoritative `raw/pages.jsonl` history, and
compact checkpoint resume head are committed in that order. Heartbeats show
process liveness but do not count as progress.

Checkpoint reconstruction and resume remain inside the collection boundary.
Resume validates the durable head against preserved raw evidence, ignores
uncheckpointed page files, restarts at the first uncommitted page, creates no
successful-bundle marker in the source partial, and completes into a separate
immutable output directory.

## Local catalog boundary

Each snapshot import is one `BEGIN IMMEDIATE` transaction. Snapshot rows are
visible only after every canonical record and relationship has been inserted
and the manifest counts have been rechecked. Exact canonical server versions
may be reused by multiple snapshots; the same server identifier and version
with a different canonical digest is rejected as a conflict.

SQLite foreign keys are enabled on every connection. The explorer keeps the
default rollback journal and durability settings because the expected workflow
has one local writer and bounded readers; WAL sidecars do not provide a useful
tradeoff for this milestone. FTS5 is detected by attempting schema creation,
with an indexed, escaped `LIKE` fallback when the linked SQLite lacks FTS5.
