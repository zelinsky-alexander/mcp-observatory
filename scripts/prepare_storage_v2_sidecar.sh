#!/usr/bin/env bash
set -euo pipefail

# Prepare a completely separate Storage v2 test state from the currently
# published catalog. This script never changes the live catalog, timers,
# portal service, Nginx, or existing evidence tree.

project_dir="${MCPO_PROJECT_DIR:-/opt/mcp-observatory/current}"
source_db="${MCPO_V2_SOURCE_DATABASE:-/var/lib/mcp-observatory/catalog/local-registry.sqlite}"
state_dir="${MCPO_V2_STATE_DIR:-/var/lib/mcp-observatory-v2}"
history_db="${MCPO_V2_HISTORY_DATABASE:-$state_dir/history/assurance-history.sqlite}"
hot_db="${MCPO_V2_HOT_DATABASE:-$state_dir/catalog/local-registry.sqlite}"
evidence_root="${MCPO_V2_EVIDENCE_ROOT:-$state_dir/evidence}"
bundle_root="${MCPO_V2_BUNDLE_ROOT:-$state_dir/evidence-bundles}"
batch_size="${MCPO_V2_BACKFILL_BATCH_SIZE:-250}"
minimum_free_multiplier="${MCPO_V2_MINIMUM_FREE_MULTIPLIER:-4}"

[[ $EUID -eq 0 ]] || { echo "run with sudo" >&2; exit 2; }
[[ -f "$source_db" ]] || { echo "source catalog not found: $source_db" >&2; exit 2; }
for required in storage_v2_mvp.py storage_v2_reconcile.py; do
  [[ -f "$project_dir/tools/$required" ]] || { echo "Storage v2 tool not found: $project_dir/tools/$required" >&2; exit 2; }
done

if [[ -e "$history_db" || -e "$hot_db" ]]; then
  echo "refusing to overwrite existing Storage v2 databases" >&2
  echo "history=$history_db" >&2
  echo "hot=$hot_db" >&2
  exit 2
fi

# Cloning + compacting/VACUUMing temporarily requires multiple catalog-sized
# files. Refuse early rather than exhausting the production filesystem.
source_bytes="$(stat -c %s "$source_db")"
state_parent="$(dirname "$state_dir")"
available_bytes="$(df -PB1 "$state_parent" | awk 'NR==2 {print $4}')"
required_bytes=$(( source_bytes * minimum_free_multiplier ))
if (( available_bytes < required_bytes )); then
  echo "insufficient free space for sidecar preparation" >&2
  echo "available_bytes=$available_bytes required_bytes=$required_bytes source_bytes=$source_bytes" >&2
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

# Recompute global finding/review counters once per distinct canonical analysis
# run. This avoids multiplying findings when a completed run is reused by more
# than one package record, then republish the corrected read model.
sudo -u mcp-refresh python3 "$project_dir/tools/storage_v2_reconcile.py" \
  --database "$history_db"
sudo -u mcp-refresh python3 "$project_dir/tools/storage_v2_mvp.py" publish \
  --history "$history_db" --hot "$hot_db"
sudo -u mcp-refresh python3 "$project_dir/tools/storage_v2_mvp.py" verify \
  --history "$history_db" --hot "$hot_db"

chgrp mcp-catalog "$history_db" "$hot_db"
chmod 0640 "$history_db" "$hot_db"

printf '\nStorage v2 sidecar prepared.\n'
printf 'history:  %s\n' "$history_db"
printf 'hot:      %s\n' "$hot_db"
printf 'evidence: %s\n' "$evidence_root"
printf 'bundles:  %s\n' "$bundle_root"
printf '\nNo existing service or timer was modified.\n'
