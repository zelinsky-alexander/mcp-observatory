#!/usr/bin/env bash
set -Eeuo pipefail

project_dir="${MCPO_PROJECT_DIR:-/opt/mcp-observatory/current}"
state_dir="${MCPO_V2_STATE_DIR:-/var/lib/mcp-observatory-v2}"
portal_url="${MCP_PORTAL_URL:-http://127.0.0.1:8081}"

echo "=== MCP v2 health ==="
echo "project: $project_dir"
echo "state:   $state_dir"
echo

echo "=== Failed units ==="
systemctl --failed --no-pager || true

echo
echo "=== Services / timers ==="
systemctl --no-pager --full status \
  mcp-portal-storage-v2.service \
  mcp-observatory-v2-refresh.timer \
  mcp-observatory-v2-static-analysis.timer \
  2>/dev/null || true

echo
echo "=== Portal smoke tests ==="
for path in / /servers /coverage; do
  curl --max-time 10 -sS -o /dev/null \
    -w "$path %{http_code} %{time_total}s\n" \
    "$portal_url$path"
done

echo
echo "=== Storage v2 files ==="
for path in \
  "$state_dir/catalog/local-registry.sqlite" \
  "$state_dir/history/assurance-history.sqlite"
do
  if [[ -e "$path" ]]; then
    ls -lh "$path" 2>/dev/null || echo "EXISTS (metadata not readable as current user): $path"
  else
    echo "MISSING: $path"
  fi
done

echo
echo "=== Filesystem ==="
df -h /
