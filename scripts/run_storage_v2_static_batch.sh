#!/usr/bin/env bash
set -euo pipefail

project_dir="${MCPO_PROJECT_DIR:-/opt/mcp-observatory/current}"
state_dir="${MCPO_V2_STATE_DIR:-/var/lib/mcp-observatory-v2}"
history_db="${MCPO_V2_HISTORY_DATABASE:-$state_dir/history/assurance-history.sqlite}"
hot_db="${MCPO_V2_HOT_DATABASE:-$state_dir/catalog/local-registry.sqlite}"
evidence_root="${MCPO_V2_EVIDENCE_ROOT:-$state_dir/evidence}"
bundle_root="${MCPO_V2_BUNDLE_ROOT:-$state_dir/evidence-bundles}"
binary="${MCPO_BINARY:-$project_dir/build/release/mcp-observatory}"
rules="${MCPO_STATIC_ANALYSIS_RULES:-$project_dir/rules/artifact-static-analysis-v1.json}"
batch_size="${MCPO_V2_BATCH_SIZE:-1}"
maximum_run_seconds="${MCPO_V2_MAXIMUM_RUN_SECONDS:-900}"
child_timeout_seconds="${MCPO_V2_CHILD_TIMEOUT_SECONDS:-300}"

for file in "$history_db" "$hot_db" "$binary" "$rules"; do
  [[ -f "$file" ]] || { echo "required file missing: $file" >&2; exit 2; }
done

exec python3 "$project_dir/tools/bulk_static_analysis_v2.py" \
  --history-database "$history_db" \
  --hot-database "$hot_db" \
  --observatory-binary "$binary" \
  --rules "$rules" \
  --evidence-root "$evidence_root" \
  --bundle-root "$bundle_root" \
  --bundle-limit "$batch_size" \
  --batch-size "$batch_size" \
  --maximum-run-seconds "$maximum_run_seconds" \
  --child-timeout-seconds "$child_timeout_seconds"
