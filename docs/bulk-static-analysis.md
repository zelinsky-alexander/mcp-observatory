# Bulk static artifact analysis

`tools/bulk_static_analysis.py` processes the Observatory catalog in bounded,
idempotent runs. It selects only package records that the current static analyzer
can potentially resolve: npm or PyPI declarations with an exact package version.

The scheduler has two simultaneous responsibilities:

1. drain the historical backlog of eligible package records that do not yet have
   a compatible completed run;
2. detect package records created by later catalog refreshes and move those new
   records to the front of the same queue.

An updated package version is represented by a new immutable server-version and
package record. It is therefore discovered and scheduled in the same way as a new
package declaration.

The scheduler does not implement package inspection itself. It invokes the fixed
`mcp-observatory analyze package` command and relies on that command's existing
artifact-level deduplication. A completed run is compatible only when its exact
artifact SHA-256, analyzer version, and ruleset version match. The analyzer may
therefore return an existing immutable run for another catalog declaration of the
same artifact.

## Run a bounded maintenance window

```bash
python3 tools/bulk_static_analysis.py \
  --database db/local-registry.sqlite \
  --observatory-binary build/release/mcp-observatory \
  --rules rules/artifact-static-analysis-v1.json \
  --evidence-root evidence \
  --batch-size 1000 \
  --maximum-run-seconds 3000 \
  --child-timeout-seconds 300 \
  --format json
```

`--batch-size` is the maximum number of package records processed in one process.
`--maximum-run-seconds` is a second, independent bound. The scheduler stops before
starting a child when the remaining maintenance window is shorter than the child
timeout. A value of zero disables the overall time bound and preserves one-batch
command-line behavior.

The JSON result includes:

- records processed in this run;
- records remaining in an actionable queue state;
- elapsed runtime;
- mean seconds per processed record for this run;
- a simple remaining-time estimate based on that observed mean;
- whether the run stopped because the queue was empty, its batch limit was
  reached, or its time budget was exhausted.

The estimate is operational guidance, not a promise. Download latency, artifact
size, failures, unsupported formats, and artifact deduplication can materially
change throughput.

Important bounds:

- `--batch-size` defaults to 25 and is capped at 1,000;
- `--maximum-attempts` defaults to 3 per package and profile;
- failed records are retried only after `--retry-failed-after-seconds`;
- each analyzer child has a wall-clock timeout and bounded combined output;
- the scheduler and Observatory writers share the catalog's
  `<database>.writer.lock` advisory lock;
- package record IDs always come from the catalog and are passed through the
  exact `--package-id` analyzer selector, never from an HTTP request or shell
  expansion;
- analyzer commands use a fixed argument vector with no shell.

## Automatic operation

The example deployment provides:

- `mcp-observatory-static-analysis.service`: one bounded maintenance window,
  configured for at most 50 minutes and at most 1,000 records;
- `mcp-observatory-static-analysis.timer`: starts a maintenance window hourly and
  shortly after boot;
- `OnSuccess=mcp-observatory-static-analysis.service` on the registry refresh
  service, so every successful catalog publication triggers analysis immediately.

This combination closes the existing backlog without requiring one unbounded
process. New package records discovered after a later refresh receive a newer
`discovered_at` value and are selected before older backlog records. The hourly
maintenance timer continues draining the historical backlog after the newly
published records are handled.

The example runs analysis as `mcp-refresh`, the existing catalog writer. This
preserves the deployed `0640 mcp-refresh:mcp-catalog` database boundary: the
public portal remains group-read-only while the writer account can append analysis
records. The writer account must also be able to invoke the Docker daemon and own
the evidence directory:

```bash
sudo usermod --append --groups docker mcp-refresh
sudo install -d -o mcp-refresh -g mcp-catalog -m 0750 \
  /var/lib/mcp-observatory/evidence
```

Group membership changes require a service restart or a new login/session. Verify
the deployed ownership and modes rather than applying the example commands
blindly.

Install the example units using paths appropriate for the deployment, then run:

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now mcp-observatory-static-analysis.timer
```

A successful registry refresh also requests the analysis service immediately. If
that service is already active, systemd does not start a duplicate instance; the
next candidate selection sees the newly published catalog records.

Docker daemon access is a major privilege boundary. A rootless Docker daemon or a
dedicated analysis host is preferable for a hardened public deployment. Until
then, the public portal must remain a separate unprivileged account with no Docker
or catalog-write access.

## Backfill duration

The exact duration cannot be known before observing the production workload. For
an illustrative backlog of 41,280 eligible records with one sequential worker:

| Mean time per selected record | Continuous processing time |
| ---: | ---: |
| 15 seconds | about 7.2 days |
| 30 seconds | about 14.3 days |
| 60 seconds | about 28.7 days |
| 120 seconds | about 57.3 days |

Hourly 50-minute maintenance windows add roughly 20% calendar overhead, so the
same examples become approximately 9, 17, 35, and 69 days. Artifact-level reuse
can shorten the effective time, while repeated network failures and timeouts can
lengthen it. The scheduler's runtime and ETA fields should replace these initial
planning estimates once real runs are available.

## Persistent scheduling state

The scheduler creates three additive tables in the catalog:

- `static_analysis_schedule_profiles`: immutable analyzer/ruleset identities;
- `static_analysis_schedule_current`: the profile selected for current coverage;
- `static_analysis_schedule_state`: one mutually exclusive state per package and
  profile.

Each state row preserves when the scheduler first discovered that package record.
Newly discovered records are selected before the older backlog, while retries are
still bounded and delayed.

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

Registry HTTP 404 responses and registry identity mismatches are terminal `unresolvable` outcomes. PyPI releases without a supported non-yanked tar-gzip source distribution are terminal `unsupported` outcomes. The scheduler preserves these states across later synchronization runs instead of consuming retries.

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
