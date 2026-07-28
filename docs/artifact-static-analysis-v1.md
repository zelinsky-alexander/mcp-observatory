# Artifact Static Analysis v1

## Trust model

`mcp-observatory analyze package` performs bounded static inspection of one
exact npm-backed MCP server package artifact. Download of npm metadata and the
published tarball may use the network. Archive extraction and code inspection
run inside a disposable Docker container with no network, a read-only root
filesystem, dropped capabilities, no-new-privileges, a non-root user, and
bounded memory, CPU, PID, and file-descriptor limits.

The host process never mounts the SQLite database or a writable evidence
directory into the analyzer container. The container emits bounded JSON on
stdout. The host validates that JSON and performs all database writes.

No package entry point is executed. No npm lifecycle script is executed.
`npm install` is never invoked.

> No static analysis result proves that an MCP server is non-malicious. The result
> describes observed properties of one exact artifact digest under one analyzer
> and ruleset version.

## What this can and cannot prove

Static analysis can observe:

- package metadata and declared lifecycle scripts;
- file-type inventory including native binaries and nested archives;
- textual Node built-in imports and selected risk-relevant APIs;
- published npm integrity verification for the exact downloaded bytes.

It cannot prove absence of malice, cannot evaluate runtime behavior, cannot
confirm that dependencies are safe, and cannot replace dynamic inspection or
human review.

## CLI

```bash
mcp-observatory analyze package \
  --database db/local-registry.sqlite \
  --server io.github.aimsise/color-engine-mcp \
  --version 1.0.1 \
  --package color-engine-mcp \
  --rules rules/artifact-static-analysis-v1.json \
  --evidence-root evidence \
  --format text
```

Also supported:

- `--format json`
- `--rules PATH` to select a versioned analysis-policy JSON document
- `--force` to ignore completed-run deduplication
- `--npm-registry-url URL` for offline fixtures or alternate registries
- archive size/count limits: `--maximum-files`,
  `--maximum-total-uncompressed-bytes`, `--maximum-individual-file-bytes`,
  `--maximum-tarball-bytes`
- `--allow-in-process-worker` for offline tests only

Internal container entrypoint:

```bash
mcp-observatory analyze-worker --tarball /in/artifact.tgz
```

## Database records

Schema version 2 extends the explorer catalog with:

- `analysis_runs`
- `analysis_artifacts`
- `analysis_findings`
- `analysis_files`
- `analysis_dependencies`
- `analysis_evidence`

Existing schema version 1 databases are migrated on the first write that needs
analysis tables. Completed runs are immutable. Failed attempts still insert an
`analysis_runs` row with `status=failed`, `error_stage`, and a bounded
`error_message`.

Finding dispositions start as `unreviewed`. Allowed values:

`unreviewed`, `expected`, `reviewed-benign`, `mitigated`, `suspicious`,
`confirmed-risk`, `false-positive`.

Severities: `info`, `low`, `medium`, `high`, `critical`.

## Evidence layout

Finalized evidence is stored under a digest path:

```text
<evidence-root>/artifacts/sha256/<aa>/<sha256>/
  artifact.tgz
  analysis-rules.json
  npm-metadata.json
  archive-inventory.json
  package-manifest.json
  files.jsonl
  dependencies.json
  findings.jsonl
  analysis-summary.json
  analyzer.log
```

Database evidence rows store relative paths only, with SHA-256, byte size, and
media type.

## Deduplication

A completed compatible run is reused when artifact SHA-256, analyzer version,
and the ruleset's declared `ruleset_version` match. Package name/version alone
is not sufficient because registry identity drift is a research target.

## External analysis rules

Finding policy is defined in
`rules/artifact-static-analysis-v1.json`. The readable, versioned document owns:

- Node built-in tokens and their severities;
- risk-API detector selection, patterns, symbols, severities, and titles;
- process-call owner aliases and documentation-context suppression markers;
- lifecycle-script classifications;
- notable file-type groups and whether they mark an artifact as containing
  native code;
- finding categories, confidence, titles, and explanations.

The C++ worker implements bounded detector mechanisms but does not assign
rule-specific severity or finding metadata. Rules are parsed with duplicate-key,
type, enum, and required-field validation before analysis. Invalid rules fail
closed.

The host stages the exact validated rules bytes beside the tarball, mounts that
file read-only into the restricted worker, requires the worker to report the
same `ruleset_version`, and stores the file as `analysis-rules.json` evidence.
Any policy change must use a new `ruleset_version`; otherwise completed-run
deduplication may intentionally reuse results produced under the previous
content carrying that version.

## Current limitation

v1 supports `registryType = npm` only, requires an exact package version, and
rejects ambiguous package selection. Unsupported registries fail closed.

## Future Web UI query surface

A later UI should join `analysis_runs` to `packages` and `server_versions`,
filter by `status`, `artifact_sha256`, severity aggregates from
`analysis_findings`, and open evidence through relative paths under the
configured evidence root. Do not treat `disposition` or severity as a safety
verdict.

## Optional live smoke test

```bash
./tests/smoke_analyze_color_engine.sh ./build/dev-debug/mcp-observatory
```

The normal test suite remains offline and deterministic.
