#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: run_storage_v2_runtime_observation.sh <server-identifier> <server-version> <package-identifier>

Runs one bounded existing runtime-discovery observation against the isolated
Storage v2 history database, then publishes/verifies the compact hot catalog.
No MCP tool is invoked by this wrapper.

Important override for side-branch testing:
  MCPO_NATIVE_GUARD_BINARY=/opt/mcp-storage-v2-test/mcp-native-guard/build/release/mcp-native-guard
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi
if [[ $# -ne 3 ]]; then
  usage >&2
  exit 2
fi

server="$1"
version="$2"
package="$3"
project_dir="${MCPO_PROJECT_DIR:-/opt/mcp-observatory/current}"
state_dir="${MCPO_V2_STATE_DIR:-/var/lib/mcp-observatory-v2}"
history_db="${MCPO_V2_HISTORY_DATABASE:-$state_dir/history/assurance-history.sqlite}"
hot_db="${MCPO_V2_HOT_DATABASE:-$state_dir/catalog/local-registry.sqlite}"
evidence_root="${MCPO_V2_RUNTIME_EVIDENCE_ROOT:-$state_dir/runtime-evidence}"
guard_binary="${MCPO_NATIVE_GUARD_BINARY:-/opt/mcp-native-guard/current/build/release/mcp-native-guard}"
runtime_image="${MCPO_RUNTIME_IMAGE:-node:22-bookworm-slim}"
timeout="${MCPO_RUNTIME_TIMEOUT_SECONDS:-180}"

for file in "$history_db" "$hot_db" "$guard_binary" "$project_dir/tools/runtime_discovery.py"; do
  [[ -f "$file" ]] || { echo "required file missing: $file" >&2; exit 2; }
done
install -d -m 0750 -o mcp-refresh -g mcp-catalog "$evidence_root"

sudo -u mcp-refresh python3 "$project_dir/tools/runtime_discovery.py" observe \
  --database "$history_db" \
  --server "$server" \
  --version "$version" \
  --package "$package" \
  --guard-binary "$guard_binary" \
  --evidence-root "$evidence_root" \
  --runtime-image "$runtime_image" \
  --timeout "$timeout"

sudo -u mcp-refresh python3 "$project_dir/tools/storage_v2_mvp.py" publish \
  --history "$history_db" --hot "$hot_db"
sudo -u mcp-refresh python3 "$project_dir/tools/storage_v2_mvp.py" verify \
  --history "$history_db" --hot "$hot_db"

echo "runtime observation published to Storage v2 hot catalog"
