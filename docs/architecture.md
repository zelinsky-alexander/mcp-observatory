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

Current commands perform no network activity and launch no external process.

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
