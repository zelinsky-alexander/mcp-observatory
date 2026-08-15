#!/usr/bin/env bash
set -Eeuo pipefail

state_dir="${MCPO_V2_STATE_DIR:-/var/lib/mcp-observatory-v2}"

echo "=== Filesystem ==="
df -h /

echo
echo "=== Storage v2 ==="
for p in \
  "$state_dir/catalog" \
  "$state_dir/history" \
  "$state_dir/evidence" \
  "$state_dir/evidence-bundles" \
  "$state_dir/registry-refresh"
do
  sudo du -sh "$p" 2>/dev/null || true
done

echo
echo "=== Databases ==="
sudo ls -lh \
  "$state_dir/catalog/local-registry.sqlite"* \
  "$state_dir/history/assurance-history.sqlite"* \
  2>/dev/null || true

echo
echo "=== Application deployments ==="
for p in \
  /opt/mcp-observatory \
  /opt/mcp-native-guard \
  /opt/mcp-observatory-guard-portal \
  /opt/mcp-storage-v2-test
 do
  sudo du -sh "$p" 2>/dev/null || true
done

echo
echo "=== Logs ==="
sudo journalctl --disk-usage || true
sudo du -sh /var/log/nginx 2>/dev/null || true

echo
echo "=== Largest MCP paths ==="
sudo du -h --max-depth=2 \
  "$state_dir" \
  /opt/mcp-observatory \
  /opt/mcp-native-guard \
  /opt/mcp-observatory-guard-portal \
  /opt/mcp-storage-v2-test \
  2>/dev/null | sort -h | tail -30
