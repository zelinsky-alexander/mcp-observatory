#!/usr/bin/env bash
set -Eeuo pipefail

# Refresh only the isolated compact v2 catalog, synchronize immutable Registry
# identities into the history store, refresh the static-analysis schedule without
# executing packages, then publish updated compact summaries back to hot.

[[ $EUID -eq 0 ]] || { echo "run as root" >&2; exit 2; }

project_dir="${MCPO_PROJECT_DIR:-/opt/mcp-observatory/current}"
state_dir="${MCPO_V2_STATE_DIR:-/var/lib/mcp-observatory-v2}"
history_db="${MCPO_V2_HISTORY_DATABASE:-$state_dir/history/assurance-history.sqlite}"
hot_db="${MCPO_V2_HOT_DATABASE:-$state_dir/catalog/local-registry.sqlite}"
runtime_dir="${MCPO_V2_REFRESH_RUNTIME_DIR:-$state_dir/registry-refresh}"
binary="${MCPO_BINARY:-$project_dir/build/release/mcp-observatory}"
rules="${MCPO_STATIC_ANALYSIS_RULES:-$project_dir/rules/artifact-static-analysis-v1.json}"
timezone="${MCPO_LOCAL_TIMEZONE:-Asia/Jerusalem}"
retention="${MCPO_V2_BACKUP_RETENTION_COUNT:-2}"

for file in "$history_db" "$hot_db" "$binary" "$rules"; do
  [[ -f "$file" ]] || { echo "required file missing: $file" >&2; exit 2; }
done
install -d -m 0750 -o mcp-refresh -g mcp-catalog "$runtime_dir"

start=$(date +%s)

echo "[storage-v2-refresh] refreshing compact hot catalog"
sudo -u mcp-refresh env \
  MCPO_PROJECT_DIR="$project_dir" \
  MCPO_BINARY="$binary" \
  MCPO_DATABASE="$hot_db" \
  MCPO_REFRESH_RUNTIME_DIR="$runtime_dir" \
  MCPO_LOCAL_TIMEZONE="$timezone" \
  MCPO_BACKUP_RETENTION_COUNT="$retention" \
  "$project_dir/scripts/daily_registry_refresh.sh"

echo "[storage-v2-refresh] synchronizing immutable Registry identities to history"
sudo -u mcp-refresh python3 "$project_dir/tools/storage_v2_registry_sync.py" \
  --hot "$hot_db" --history "$history_db"

echo "[storage-v2-refresh] synchronizing analysis schedule without executing artifacts"
sudo -u mcp-refresh python3 "$project_dir/tools/storage_v2_sync_schedule.py" \
  --database "$history_db" --rules "$rules"

echo "[storage-v2-refresh] reconciling canonical coverage"
sudo -u mcp-refresh python3 "$project_dir/tools/storage_v2_reconcile.py" \
  --database "$history_db"

echo "[storage-v2-refresh] publishing updated summaries"
sudo -u mcp-refresh python3 "$project_dir/tools/storage_v2_mvp.py" publish \
  --history "$history_db" --hot "$hot_db"

sudo -u mcp-refresh python3 "$project_dir/tools/storage_v2_mvp.py" verify \
  --history "$history_db" --hot "$hot_db"

elapsed=$(( $(date +%s) - start ))
echo "[storage-v2-refresh] completed in ${elapsed}s"
