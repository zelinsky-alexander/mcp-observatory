#!/usr/bin/env bash
set -euo pipefail

# Prepare a completely separate Storage v2 test state from the currently
# published catalog.  This script never changes the live catalog, timers,
# portal service, Nginx, or existing evidence tree.

project_dir="${MCPO_PROJECT_DIR:-/opt/mcp-observatory/current}"
source_db="${MCPO_V2_SOURCE_DATABASE:-/var/lib/mcp-observatory/catalog/local-registry.sqlite}"
state_dir="${MCPO_V2_STATE_DIR:-/var/lib/mcp-observatory-v2}"
history_db="${MCPO_V2_HISTORY_DATABASE:-$state_dir/history/assurance-history.sqlite}"
hot_db="${MCPO_V2_HOT_DATABASE:-$state_dir/catalog/local-registry.sqlite}"
evidence_root="${MCPO_V2_EVIDENCE_ROOT:-$state_dir/evidence}"
bundle_root="${MCPO_V2_BUNDLE_ROOT:-$state_dir/evidence-bundles}"
batch_size="${MCPO_V2_BACKFILL_BATCH_SIZE:-250}"

[[ -f "$source_db" ]] || { echo "source catalog not found: $source_db" >&2; exit 2; }
[[ -f "$project_dir/tools/storage_v2_mvp.py" ]] || { echo "Storage v2 tool not found in $project_dir" >&2; exit 2; }

if [[ -e "$history_db" || -e "$hot_db" ]]; then
  echo "refusing to overwrite existing Storage v2 databases" >&2
  echo "history=$history_db" >&2
  echo "hot=$hot_db" >&2
  exit 2
fi

install -d -m 0750 -o mcp-refresh -g mcp-catalog \
  "$state_dir" "$(dirname "$history_db")" "$(dirname "$hot_db")"
install -d -m 0750 -o mcp-refresh -g mcp-catalog "$evidence_root" "$bundle_root"

sudo -u mcp-refresh python3 "$project_dir/tools/storage_v2_mvp.py" prepare \
  --source "$source_db" \
  --history "$history_db" \
  --hot "$hot_db" \
  --batch-size "$batch_size"

chgrp mcp-catalog "$history_db" "$hot_db"
chmod 0640 "$history_db" "$hot_db"

printf '\nStorage v2 sidecar prepared.\n'
printf 'history: %s\n' "$history_db"
printf 'hot:     %s\n' "$hot_db"
printf 'evidence:%s\n' "$evidence_root"
printf 'bundles: %s\n' "$bundle_root"
printf '\nNo existing service or timer was modified.\n'
