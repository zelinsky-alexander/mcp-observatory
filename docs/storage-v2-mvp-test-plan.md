# Storage v2 MVP production-side test plan

This plan validates Storage v2 **side by side** on the existing Lightsail host. It does not replace the current catalog, public portal, timers, Nginx, or Cloudflare configuration.

## Branches

Use these branches for the isolated test checkout:

```text
mcp-observatory               storage-v2-foundation
mcp-observatory-guard-portal  storage-v2-mvp
mcp-native-guard              storage-v2-mvp
```

The Native Guard branch intentionally starts from current `main`; Storage v2 reuses the existing bounded `inspect` runtime-discovery contract rather than changing Guard execution semantics.

## 0. Preconditions

The existing static-analysis scheduler must remain stopped/disabled while the MVP is being validated. The v2 scripts never write `/var/lib/mcp-observatory/catalog/local-registry.sqlite` or the existing evidence tree.

Confirm:

```bash
systemctl status mcp-observatory-static-analysis.service \
  mcp-observatory-static-analysis.timer --no-pager
systemctl show mcp-observatory-refresh.service -p OnSuccess
sudo docker ps

df -h /
```

`OnSuccess=` must remain empty. No static-analysis container should be running.

## 1. Clone side branches

Use a separate test checkout; do not repoint `/opt/.../current`.

```bash
sudo mkdir -p /opt/mcp-storage-v2-test
sudo chown "$USER":"$USER" /opt/mcp-storage-v2-test
cd /opt/mcp-storage-v2-test

git clone --branch storage-v2-foundation --single-branch \
  https://github.com/zelinsky-alexander/mcp-observatory.git
git clone --branch storage-v2-mvp --single-branch \
  https://github.com/zelinsky-alexander/mcp-observatory-guard-portal.git
git clone --branch storage-v2-mvp --single-branch \
  https://github.com/zelinsky-alexander/mcp-native-guard.git
```

Record exact test SHAs:

```bash
for repo in mcp-observatory mcp-observatory-guard-portal mcp-native-guard; do
  git -C "$repo" rev-parse HEAD
done
```

## 2. Build and run repository tests

Observatory:

```bash
cd /opt/mcp-storage-v2-test/mcp-observatory
cmake --preset release
cmake --build --preset release --parallel 2
ctest --preset release --output-on-failure
```

Portal:

```bash
cd /opt/mcp-storage-v2-test/mcp-observatory-guard-portal
python3 -m compileall -q mcp_portal tests
python3 -m unittest discover -s tests -v
bash -n scripts/install_storage_v2_sidecar_portal.sh
```

Native Guard:

```bash
cd /opt/mcp-storage-v2-test/mcp-native-guard
cmake --preset release
cmake --build --preset release --parallel 2
ctest --preset release --output-on-failure
```

Do not continue if any repository test fails.

## 3. Prepare isolated Storage v2 state

The script refuses to overwrite existing v2 databases and checks free space before cloning. Default test state is entirely under `/var/lib/mcp-observatory-v2`.

```bash
cd /opt/mcp-storage-v2-test/mcp-observatory
sudo MCPO_PROJECT_DIR="$PWD" ./scripts/prepare_storage_v2_sidecar.sh
```

Expected layout:

```text
/var/lib/mcp-observatory-v2/
  history/assurance-history.sqlite
  catalog/local-registry.sqlite
  evidence/
  evidence-bundles/
```

Verify:

```bash
sudo MCPO_PROJECT_DIR="$PWD" ./scripts/verify_storage_v2_sidecar.sh
sudo du -sh /var/lib/mcp-observatory-v2/*
ls -lh /var/lib/mcp-observatory-v2/history/assurance-history.sqlite \
       /var/lib/mcp-observatory-v2/catalog/local-registry.sqlite
```

Acceptance at this stage:

- both SQLite integrity checks are `ok`;
- snapshot/server/package counts match;
- all `analysis_runs` have v2 run summaries;
- v2 coverage is materialized;
- the hot DB has no bulk v1 file/finding/dependency/evidence rows;
- the existing `/var/lib/mcp-observatory` files have not changed as a result of preparation.

## 4. Start the local-only v2 portal on port 8081

```bash
cd /opt/mcp-storage-v2-test/mcp-observatory-guard-portal
sudo MCP_PORTAL_V2_PROJECT_DIR="$PWD" \
  ./scripts/install_storage_v2_sidecar_portal.sh --start
```

The sidecar reads:

```text
hot/common pages: /var/lib/mcp-observatory-v2/catalog/local-registry.sqlite
bounded detail pages: /var/lib/mcp-observatory-v2/history/assurance-history.sqlite
```

It binds only `127.0.0.1:8081` and is not added to Nginx or Cloudflare.

Check:

```bash
systemctl status mcp-portal-storage-v2.service --no-pager
ss -ltnp | grep ':8081'
```

## 5. Compare portal results and latency

Existing production portal remains on `8080`; sidecar is `8081`.

```bash
for port in 8080 8081; do
  echo "=== PORT $port ==="
  for path in / /servers /coverage /snapshots /reports/ecosystems; do
    curl -sS -o /dev/null \
      -w "$path HTTP=%{http_code} time=%{time_total}s\n" \
      --max-time 120 "http://127.0.0.1:$port$path"
  done
done
```

Also compare current snapshot identity:

```bash
for db in \
  /var/lib/mcp-observatory/catalog/local-registry.sqlite \
  /var/lib/mcp-observatory-v2/catalog/local-registry.sqlite; do
  sudo -u mcp-portal sqlite3 "file:$db?mode=ro" \
    "SELECT id,completed_at,snapshot_sha256 FROM snapshots ORDER BY completed_at DESC,id DESC LIMIT 1;"
done
```

Target for the common v2 routes is comfortably below the public timeout; `/` and `/coverage` should ideally be sub-second at the present data volume. Server/analysis detail pages may be slower because they deliberately query the full history DB, but they are bounded to a selected server/run.

## 6. One-record static-analysis writer test

The writer operates only on the v2 history DB and separate v2 evidence root, then publishes compact summaries to hot.

```bash
cd /opt/mcp-storage-v2-test/mcp-observatory
sudo MCPO_PROJECT_DIR="$PWD" MCPO_V2_BATCH_SIZE=1 \
  ./scripts/run_storage_v2_static_batch.sh
```

Then:

```bash
sudo MCPO_PROJECT_DIR="$PWD" ./scripts/verify_storage_v2_sidecar.sh
```

Confirm no new detail rows reached hot:

```bash
sudo -u mcp-portal sqlite3 \
  "file:/var/lib/mcp-observatory-v2/catalog/local-registry.sqlite?mode=ro" <<'SQL'
SELECT 'analysis_files',COUNT(*) FROM analysis_files;
SELECT 'analysis_findings',COUNT(*) FROM analysis_findings;
SELECT 'analysis_dependencies',COUNT(*) FROM analysis_dependencies;
SELECT 'analysis_evidence',COUNT(*) FROM analysis_evidence;
SELECT 'v2_summaries',COUNT(*) FROM analysis_v2_run_summaries;
SQL
```

If the one-record run is correct, repeat with a bounded batch of 5:

```bash
sudo MCPO_PROJECT_DIR="$PWD" MCPO_V2_BATCH_SIZE=5 \
  ./scripts/run_storage_v2_static_batch.sh
```

Re-run verification and portal timings.

## 7. Evidence bundle MVP

The v2 static wrapper can create content-addressed deterministic evidence bundles. The bundle excludes `artifact.tgz` and `analysis-rules.json`; it does not delete the working evidence directory during the MVP.

Inspect:

```bash
find /var/lib/mcp-observatory-v2/evidence-bundles -type f | head
sudo du -sh /var/lib/mcp-observatory-v2/evidence \
             /var/lib/mcp-observatory-v2/evidence-bundles
```

For one bundle:

```bash
bundle="$(find /var/lib/mcp-observatory-v2/evidence-bundles -type f -name '*.tar.gz' | head -1)"
tar -tzf "$bundle"
tar -tzf "$bundle" | grep -E '(^|/)(artifact\.tgz|analysis-rules\.json)$' && \
  echo 'ERROR: excluded content present' || true
sha256sum "$bundle"
```

No legacy evidence cleanup is part of the MVP acceptance test.

## 8. Isolated registry refresh test

Only after portal/static tests pass:

```bash
cd /opt/mcp-storage-v2-test/mcp-observatory
sudo MCPO_PROJECT_DIR="$PWD" ./scripts/run_storage_v2_registry_refresh.sh
```

This refreshes the compact v2 hot catalog, synchronizes immutable registry identities into history, synchronizes the static schedule **without executing artifacts**, reconciles coverage, republishes summaries, and verifies the result.

Acceptance:

- v2 refresh completes within the service/operator test budget;
- a valid new snapshot is published if upstream changed;
- history and hot registry identities agree;
- no static package execution occurs;
- existing production catalog/timer state is untouched.

## 9. Native Guard runtime-discovery test

Use the existing bounded exact npm `stdio` runtime-observation path only after static/refresh MVP checks pass. Point it at the v2 history/control DB and the side-branch Native Guard binary. Do not enable broad tool invocation.

The repository wrapper is:

```bash
cd /opt/mcp-storage-v2-test/mcp-observatory
./scripts/run_storage_v2_runtime_observation.sh --help
```

Select one known eligible package explicitly and run the wrapper with its required arguments. Then publish/verify the v2 hot read model and inspect the corresponding runtime observation page on port 8081.

## 10. Stop / rollback

Stopping the sidecar does not affect production:

```bash
sudo systemctl stop mcp-portal-storage-v2.service
sudo systemctl disable mcp-portal-storage-v2.service
```

Do **not** remove `/var/lib/mcp-observatory-v2` until test results have been reviewed. If the MVP is rejected, the complete sidecar state and unit can be removed later without touching `/var/lib/mcp-observatory`.

## MVP acceptance gates

1. Repository CI and on-host tests pass for all three pinned branches.
2. No existing production service/config/database/evidence path is modified by v2 preparation.
3. History and hot DBs pass `integrity_check`.
4. Registry and snapshot identities match expected production state.
5. Coverage counts are materialized and canonical reused analysis runs are not double-counted in finding totals.
6. `/`, `/coverage`, `/servers`, `/snapshots`, and ecosystem reports are fast from the hot DB.
7. Bounded detail routes can read full history without putting global history scans back on common pages.
8. One-record then five-record static analysis updates history + hot summaries correctly.
9. Hot DB remains free of bulk v1 detail rows after writer tests.
10. Evidence bundles are deterministic/content-addressed and exclude package archives/rules copies by default.
11. Isolated registry refresh works on the compact catalog and does not trigger package execution.
12. One Native Guard runtime observation can be recorded and surfaced without tool invocation.

Only after all gates pass should cutover/timer migration and controlled cleanup of the current 22 GB legacy evidence tree be designed.
