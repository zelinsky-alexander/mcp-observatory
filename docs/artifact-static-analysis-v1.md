# Artifact Static Analysis v1

## Trust model

`mcp-observatory analyze package` performs bounded static inspection of one
exact npm package or PyPI source-distribution artifact. Download of registry
metadata and the published tarball may use the network. Archive extraction and
code inspection run inside a disposable Docker container with no network, a
read-only root filesystem, dropped capabilities, no-new-privileges, a non-root
user, and bounded memory, CPU, PID, and file-descriptor limits.

The host process never mounts the SQLite database or a writable evidence
directory into the analyzer container. The container emits bounded JSON on
stdout. The host validates that JSON and performs all database writes.

No package entry point or lifecycle script is executed. Package installation is
never invoked.

> No static analysis result proves that an MCP server is non-malicious. The result
> describes observed properties of one exact artifact digest under one analyzer
> and ruleset version.

## What this can and cannot prove

Static analysis can observe:

- npm `package.json` metadata and declared lifecycle scripts;
- PyPI `PKG-INFO` identity and `Requires-Dist` declarations;
- file-type inventory including native binaries and nested archives;
- textual Node built-in imports and selected risk-relevant APIs;
- published registry integrity verification for the exact downloaded bytes.

The current text detectors recognize Node/JavaScript APIs. PyPI support adds
bounded archive inventory, native-file classification, metadata extraction,
and integrity verification; it does not yet detect Python-specific risky APIs.

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
- `--pypi-registry-url URL` for offline fixtures or alternate registries
- archive size/count limits: `--maximum-files`,
  `--maximum-total-uncompressed-bytes`, `--maximum-individual-file-bytes`,
  `--maximum-tarball-bytes`
- `--allow-in-process-worker` for offline tests only

Internal container entrypoint:

```bash
mcp-observatory analyze-worker --registry npm --tarball /in/artifact.tgz
```

## Database records

Schema version 2 extends the explorer catalog with:

- `analysis_runs`
- `analysis_artifacts`
- `analysis_findings`
- `analysis_files`
- `analysis_dependencies`
- `analysis_evidence`
- `analysis_finding_reviews` (schema version 3)

Existing schema version 1 databases are migrated on the first write that needs
analysis tables. Schema version 2 databases are migrated on the next
Observatory write that requires the current schema. Completed analyzer output is immutable; the human review disposition is
the only finding field changed after analysis, and every change has an
append-only audit row. Failed attempts still insert an
`analysis_runs` row with `status=failed`, `error_stage`, and a bounded
`error_message`.

Finding dispositions start as `unreviewed`. Allowed values:

`unreviewed`, `expected`, `reviewed-benign`, `mitigated`, `suspicious`,
`confirmed-risk`, `false-positive`.

Severities: `info`, `low`, `medium`, `high`, `critical`.

Record an explicit review transition with optimistic concurrency:

```bash
mcp-observatory review finding \
  --database db/local-registry.sqlite \
  --finding-id 598 \
  --expected-disposition unreviewed \
  --disposition expected \
  --reviewer local-reviewer \
  --format json
```

Review never assigns a generic safety verdict. The new disposition must be one
of the documented review values, the expected disposition must still match,
and the disposition update and audit insertion occur in one transaction.

## Evidence layout

Finalized evidence is stored under a digest path:

```text
<evidence-root>/artifacts/sha256/<aa>/<sha256>/
  artifact.tgz
  analysis-rules.json
  registry-metadata.json
  archive-inventory.json
  package-manifest.json
  files.jsonl
  dependencies.json
  findings.jsonl
  analysis-summary.json
  analyzer.log
```

Read the verified text member associated with an existing finding:

```bash
mcp-observatory evidence finding-source \
  --database db/local-registry.sqlite \
  --evidence-root evidence \
  --finding-id 598 \
  --format json
```

The command resolves the archive path from the finding ID, requires matching
`analysis_files` and `analysis_evidence` records, verifies artifact and member
digests and sizes, rejects non-regular or duplicate members, and bounds archive
and displayed source bytes. Files larger than the display limit return a
verified window centered on the finding symbol or evidence, with explicit
leading/trailing truncation metadata. It never extracts the archive or executes
package content. After the bounded view indicates truncation, `--format raw`
returns the complete verified UTF-8 member for a controlled download; the
archive and individual-file analysis limits still apply.

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

v1 supports `registryType = npm` and tar-gzip PyPI source distributions,
requires an exact package version, and rejects ambiguous package or PyPI sdist
selection. Wheels, zip sdists, yanked files, and unsupported registries fail
closed. Python-specific source detectors are not yet implemented.

## Web UI query surface

A later UI should join `analysis_runs` to `packages` and `server_versions`,
filter by `status`, `artifact_sha256`, severity aggregates from
`analysis_findings`, and invoke the bounded finding-source interface with an
existing internal finding ID. Review writes must use the audited review
interface rather than direct UI writes to SQLite. Do not treat `disposition` or
severity as a safety verdict.

## Optional live smoke test

```bash
./tests/smoke_analyze_color_engine.sh ./build/dev-debug/mcp-observatory
```

The normal test suite remains offline and deterministic.
