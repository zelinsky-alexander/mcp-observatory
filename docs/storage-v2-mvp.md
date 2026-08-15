# Storage v2 side-by-side MVP

This branch implements a production-testable Storage v2 without modifying the existing live catalog, portal service, timers, or evidence tree.

## Goals

- Keep the public/operational catalog small and query-bounded.
- Keep detailed static-analysis rows in a separate history/control database.
- Publish compact run/rule/coverage summaries into the hot catalog.
- Keep exact artifact bytes out of the default durable v2 evidence bundle.
- Preserve detailed JSON evidence in deterministic compressed bundles.
- Reuse the existing static analyzer and existing Native Guard `inspect` runtime-discovery path during the MVP.

The project boundary remains: Observatory owns authoritative research data, the portal is read-only, and untrusted MCP execution belongs in the restricted worker/runtime path. This MVP does not claim that an observed server is safe.

## Side-by-side state

Default paths:

```text
/var/lib/mcp-observatory-v2/
  history/assurance-history.sqlite    detailed/control database
  catalog/local-registry.sqlite       compact portal read model
  evidence/                           new analysis working evidence only
  evidence-bundles/                   compressed detailed evidence
```

The existing `/var/lib/mcp-observatory` tree is not changed.

## Milestone A — prepare isolated v2 state

From a checkout/build of this branch:

```bash
sudo MCPO_PROJECT_DIR="$PWD" ./scripts/prepare_storage_v2_sidecar.sh
sudo MCPO_PROJECT_DIR="$PWD" ./scripts/verify_storage_v2_sidecar.sh
```

`prepare` uses SQLite's online backup API to clone the published catalog, installs/backfills Storage v2 summaries in the history clone, creates a second clone, removes bulky v1 detail rows from the hot clone, and VACUUMs only the new hot clone.

The hot catalog preserves the existing registry schema and `analysis_runs`, but clears these v1 detail tables:

- `analysis_files`
- `analysis_findings`
- `analysis_dependencies`
- `analysis_evidence`
- `analysis_artifacts`
- `analysis_finding_reviews`
- `static_analysis_schedule_state`

Storage v2 summary tables remain.

## Milestone B — portal read-model test

Run the portal side branch on `127.0.0.1:8081` with:

```text
MCP_PORTAL_DATABASE=/var/lib/mcp-observatory-v2/catalog/local-registry.sqlite
MCP_PORTAL_HOST=127.0.0.1
MCP_PORTAL_PORT=8081
MCP_PORTAL_MODE=public-readonly
```

Do not put this service behind public Nginx/Cloudflare until local route timings and output equivalence are accepted.

Expected checks:

```bash
for path in / /servers /coverage /snapshots /reports/ecosystems; do
  curl -sS -o /dev/null -w "$path HTTP=%{http_code} time=%{time_total}s\n" \
    --max-time 30 "http://127.0.0.1:8081$path"
done
```

Compare with the existing portal on port 8080.

## Milestone C — one-record static writer test

The v2 writer runs the existing static analyzer against the **history database**, never the hot portal catalog. SQLite triggers materialize compact summaries, which are then published to the hot catalog.

```bash
sudo MCPO_PROJECT_DIR="$PWD" MCPO_V2_BATCH_SIZE=1 \
  ./scripts/run_storage_v2_static_batch.sh
```

After a successful run:

```bash
sudo MCPO_PROJECT_DIR="$PWD" ./scripts/verify_storage_v2_sidecar.sh
```

Increase to a small bounded batch such as 5 only after the single-record result is verified.

## Evidence policy in the MVP

The v2 analysis evidence root is separate from existing evidence. `storage_v2_mvp.py bundle-evidence` creates deterministic `.tar.gz` bundles with normalized tar metadata. By default it excludes:

- `artifact.tgz`
- `analysis-rules.json`

The artifact is still hashed by the analyzer; the default durable bundle stores derived evidence rather than acting as a registry mirror. The ruleset remains identified by the analyzer/ruleset identity and should ultimately be retained once per ruleset digest.

The MVP does **not** delete the working evidence directory automatically. Cleanup/retention must remain an explicit later action until bundle verification and recovery tests pass.

## Milestone D — runtime discovery

Existing runtime discovery already follows the immediate discovery milestone: exact npm `stdio`, bounded Docker execution, `mcp-native-guard inspect`, no tool invocation, canonical inventory, and tool-definition hashes. During the side-by-side test, runtime discovery can continue against the history/control database and its `runtime_observation_runs` / `runtime_observation_tools` rows can be published into the hot catalog with:

```bash
python3 tools/storage_v2_mvp.py publish \
  --history /var/lib/mcp-observatory-v2/history/assurance-history.sqlite \
  --hot /var/lib/mcp-observatory-v2/catalog/local-registry.sqlite
```

No broad controlled tool invocation is introduced by Storage v2.

## Production-test acceptance gates

Before replacing any existing service:

1. Existing service/timers remain untouched and healthy.
2. Hot/history integrity checks return `ok`.
3. Registry counts match between history and hot.
4. `analysis_runs` and Storage v2 summary counts match.
5. Coverage summary matches history.
6. Portal sidecar returns the same current registry/snapshot identity as production.
7. Dashboard and coverage are sub-second or comfortably below the public timeout under current data volume.
8. One-record then small-batch v2 static analysis updates history and hot summaries correctly without adding detailed rows to the hot catalog.
9. Evidence bundle hashes are stable and bundles contain no `artifact.tgz` or copied rules file.
10. No existing live database/evidence path is written by the v2 test.

## Not yet automated

The side-by-side MVP deliberately does not install a v2 timer, modify the existing refresh timer, or change Nginx. Registry-refresh synchronization between a future authoritative hot catalog and the historical store is the next cutover milestone after the isolated MVP is proven.
