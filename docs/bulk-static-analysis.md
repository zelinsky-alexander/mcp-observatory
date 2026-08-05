# Bulk static artifact analysis

`tools/bulk_static_analysis.py` processes the Observatory catalog in bounded,
idempotent batches. It selects only package records that the current static
analyzer can potentially resolve: npm or PyPI declarations with an exact package
version.

The scheduler does not implement package inspection itself. It invokes the fixed
`mcp-observatory analyze package` command and relies on that command's existing
artifact-level deduplication. A completed run is compatible only when its exact
artifact SHA-256, analyzer version, and ruleset version match. The analyzer may
therefore return an existing immutable run for another catalog declaration of the
same artifact.

## Run a batch

```bash
python3 tools/bulk_static_analysis.py \
  --database db/local-registry.sqlite \
  --observatory-binary build/dev-debug/mcp-observatory \
  --rules rules/artifact-static-analysis-v1.json \
  --evidence-root evidence \
  --batch-size 25 \
  --format json
```

Important bounds:

- `--batch-size` defaults to 25 and is capped at 1,000.
- `--maximum-attempts` defaults to 3 per package and profile.
- failed records are retried only after `--retry-failed-after-seconds`;
- each analyzer child has a wall-clock timeout and bounded combined output;
- the scheduler and Observatory writers share the catalog's
  `<database>.writer.lock` advisory lock;
- package identifiers, versions, and server identifiers always come from the
  catalog, never from an HTTP request or shell expansion;
- analyzer commands use a fixed argument vector with no shell.

## Persistent scheduling state

The scheduler creates three additive tables in the catalog:

- `static_analysis_schedule_profiles`: immutable analyzer/ruleset identities;
- `static_analysis_schedule_current`: the profile selected for current coverage;
- `static_analysis_schedule_state`: one mutually exclusive state per package and
  profile.

States are:

- `eligible`: supported declaration that has not yet been selected;
- `running`: a bounded analyzer child is in progress;
- `completed`: an authoritative compatible `analysis_runs` row was verified;
- `failed`: the analyzer failed and may be retried within configured limits;
- `unsupported`: the registry or resolved artifact type is not supported;
- `unresolvable`: an exact package selection cannot currently be resolved.

A stale `running` record is changed to `failed` when the next scheduler process
starts. Errors and reasons are stored as bounded text. Successful child output is
not trusted by itself: the referenced `analysis_runs` row, artifact digest,
analyzer identity, and ruleset identity are verified in SQLite before the package
is marked completed.

## Coverage interpretation

The public portal reports the selected profile as **Static artifact coverage**:

- eligible package records;
- successfully analyzed;
- failed attempts without a compatible completion;
- unsupported or unresolvable;
- never attempted;
- unique artifact SHA-256 values analyzed.

Coverage records observable properties of exact published artifacts. It is not a
safety certification and does not include runtime discovery, controlled MCP tool
invocation, or human review unless those layers are reported separately.
