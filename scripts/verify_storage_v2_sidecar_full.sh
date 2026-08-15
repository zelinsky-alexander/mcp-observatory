#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
default_project_dir="$(cd -- "$script_dir/.." && pwd)"
project_dir="${MCPO_PROJECT_DIR:-$default_project_dir}"
state_dir="${MCPO_V2_STATE_DIR:-/var/lib/mcp-observatory-v2}"
history_db="${MCPO_V2_HISTORY_DATABASE:-$state_dir/history/assurance-history.sqlite}"
hot_db="${MCPO_V2_HOT_DATABASE:-$state_dir/catalog/local-registry.sqlite}"

python3 "$project_dir/tools/storage_v2_mvp.py" verify \
  --history "$history_db" \
  --hot "$hot_db"

echo
ls -lh "$history_db" "$hot_db"
echo
for db in "$history_db" "$hot_db"; do
  echo "=== FULL INTEGRITY CHECK: $db ==="
  sqlite3 "file:$db?mode=ro" "PRAGMA integrity_check; SELECT COUNT(*) FROM analysis_v2_run_summaries; SELECT COUNT(*) FROM analysis_v2_coverage_summary;"
done
