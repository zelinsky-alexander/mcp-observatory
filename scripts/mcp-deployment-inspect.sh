#!/usr/bin/env bash
set -Eeuo pipefail

state_dir="${MCPO_V2_STATE_DIR:-/var/lib/mcp-observatory-v2}"

echo "=== Current release links ==="
for p in \
  /opt/mcp-observatory/current \
  /opt/mcp-native-guard/current \
  /opt/mcp-observatory-guard-portal/current
 do
  printf '%-48s -> ' "$p"
  readlink -f "$p" 2>/dev/null || echo MISSING
done

echo
echo "=== Deployed commits ==="
for d in \
  /opt/mcp-observatory/current \
  /opt/mcp-native-guard/current \
  /opt/mcp-observatory-guard-portal/current
 do
  if sudo test -d "$d/.git"; then
    echo "$d"
    sudo git -C "$d" status --short --branch --untracked-files=no
    echo "HEAD: $(sudo git -C "$d" rev-parse HEAD)"
    if sudo test -r "$d/DEPLOYED_COMMIT"; then
      printf 'DEPLOYED_COMMIT: '; sudo cat "$d/DEPLOYED_COMMIT"
    fi
  elif sudo test -r "$d/DEPLOYED_COMMIT"; then
    echo "$d"
    printf 'DEPLOYED_COMMIT: '; sudo cat "$d/DEPLOYED_COMMIT"
  fi
done

echo
echo "=== MCP systemd source paths ==="
sudo grep -R -nE \
  'WorkingDirectory=|ExecStart=|MCPO_PROJECT_DIR=|MCPO_V2_STATE_DIR=|/opt/mcp' \
  /etc/systemd/system/mcp-* 2>/dev/null || true

echo
echo "=== Services / timers ==="
systemctl list-units --all --no-pager 'mcp-*' || true
systemctl list-timers --all --no-pager 'mcp-*' || true

echo
echo "=== Listeners ==="
sudo ss -ltnp | grep -E '(:80|:443|:8080|:8081)\b' || true

echo
echo "=== Storage v2 databases ==="
sudo ls -lh \
  "$state_dir/catalog/local-registry.sqlite" \
  "$state_dir/history/assurance-history.sqlite" \
  2>/dev/null || true

echo
echo "=== Git deployments under /opt ==="
sudo find \
  /opt/mcp-observatory \
  /opt/mcp-native-guard \
  /opt/mcp-observatory-guard-portal \
  -maxdepth 4 -type d -name .git -printf '%h\n' 2>/dev/null | sort || true
