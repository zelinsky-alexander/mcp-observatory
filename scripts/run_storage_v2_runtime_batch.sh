#!/usr/bin/env bash
set -euo pipefail

project_dir="${MCPO_PROJECT_DIR:-/opt/mcp-observatory/current}"
state_dir="${MCPO_V2_STATE_DIR:-/var/lib/mcp-observatory-v2}"
history_db="${MCPO_V2_HISTORY_DATABASE:-$state_dir/history/assurance-history.sqlite}"
hot_db="${MCPO_V2_HOT_DATABASE:-$state_dir/catalog/local-registry.sqlite}"
evidence_root="${MCPO_V2_RUNTIME_EVIDENCE_ROOT:-$state_dir/runtime-evidence}"
guard_root="${MCPO_NATIVE_GUARD_ROOT:-/opt/mcp-native-guard/current}"
guard_binary="${MCPO_NATIVE_GUARD_BINARY:-$guard_root/build/release/mcp-native-guard}"
probe_profile="${MCPO_RUNTIME_PROBE_PROFILE:-$guard_root/profiles/observatory-discovery-v1.json}"
remote_probe_profile="${MCPO_REMOTE_RUNTIME_PROBE_PROFILE:-$guard_root/profiles/observatory-remote-discovery-v1.json}"
runtime_image="${MCPO_RUNTIME_IMAGE:-auto-node-v1}"
batch_size="${MCPO_RUNTIME_BATCH_SIZE:-10}"
maximum_run_seconds="${MCPO_RUNTIME_MAXIMUM_RUN_SECONDS:-3000}"
phase_timeout_seconds="${MCPO_RUNTIME_PHASE_TIMEOUT_SECONDS:-180}"
child_timeout_seconds="${MCPO_RUNTIME_CHILD_TIMEOUT_SECONDS:-720}"
maximum_attempts="${MCPO_RUNTIME_MAXIMUM_ATTEMPTS:-3}"
retry_failed_after_seconds="${MCPO_RUNTIME_RETRY_FAILED_AFTER_SECONDS:-86400}"
stale_running_after_seconds="${MCPO_RUNTIME_STALE_RUNNING_AFTER_SECONDS:-7200}"
remote_batch_size="${MCPO_REMOTE_RUNTIME_BATCH_SIZE:-5}"
remote_maximum_run_seconds="${MCPO_REMOTE_RUNTIME_MAXIMUM_RUN_SECONDS:-300}"
remote_phase_timeout_seconds="${MCPO_REMOTE_RUNTIME_PHASE_TIMEOUT_SECONDS:-15}"
remote_child_timeout_seconds="${MCPO_REMOTE_RUNTIME_CHILD_TIMEOUT_SECONDS:-45}"
overall_maximum_run_seconds="${MCPO_RUNTIME_OVERALL_MAXIMUM_RUN_SECONDS:-3150}"
publication_reserve_seconds="${MCPO_RUNTIME_PUBLICATION_RESERVE_SECONDS:-120}"

for file in \
  "$history_db" \
  "$hot_db" \
  "$guard_binary" \
  "$probe_profile" \
  "$remote_probe_profile" \
  "$project_dir/tools/bulk_runtime_discovery_v2.py" \
  "$project_dir/tools/bulk_runtime_discovery_auto.py" \
  "$project_dir/tools/runtime_discovery_auto.py" \
  "$project_dir/tools/runtime_discovery.py" \
  "$project_dir/tools/bulk_remote_runtime_discovery.py" \
  "$project_dir/tools/remote_runtime_discovery.py"
do
  [[ -f "$file" ]] || { echo "required file missing: $file" >&2; exit 2; }
done

mkdir -p "$evidence_root"

exec python3 "$project_dir/tools/bulk_runtime_discovery_v2.py" \
  --history-database "$history_db" \
  --hot-database "$hot_db" \
  --guard-binary "$guard_binary" \
  --probe-profile "$probe_profile" \
  --remote-probe-profile "$remote_probe_profile" \
  --evidence-root "$evidence_root" \
  --runtime-image "$runtime_image" \
  --batch-size "$batch_size" \
  --maximum-run-seconds "$maximum_run_seconds" \
  --maximum-attempts "$maximum_attempts" \
  --retry-failed-after-seconds "$retry_failed_after_seconds" \
  --stale-running-after-seconds "$stale_running_after_seconds" \
  --phase-timeout-seconds "$phase_timeout_seconds" \
  --child-timeout-seconds "$child_timeout_seconds" \
  --remote-batch-size "$remote_batch_size" \
  --remote-maximum-run-seconds "$remote_maximum_run_seconds" \
  --remote-phase-timeout-seconds "$remote_phase_timeout_seconds" \
  --remote-child-timeout-seconds "$remote_child_timeout_seconds" \
  --overall-maximum-run-seconds "$overall_maximum_run_seconds" \
  --publication-reserve-seconds "$publication_reserve_seconds"
