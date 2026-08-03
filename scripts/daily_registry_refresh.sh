#!/usr/bin/env bash

set -Eeuo pipefail
umask 077

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
project_dir="$(dirname -- "$script_dir")"
project_dir="${MCPO_PROJECT_DIR:-$project_dir}"
binary="${MCPO_BINARY:-$project_dir/build/release/mcp-observatory}"
database="${MCPO_DATABASE:-$project_dir/db/local-registry.sqlite}"
runtime_dir="${MCPO_REFRESH_RUNTIME_DIR:-$project_dir/runtime/registry-refresh}"
local_timezone="${MCPO_LOCAL_TIMEZONE:-UTC}"
backup_retention_count="${MCPO_BACKUP_RETENTION_COUNT:-2}"

exec python3 "$project_dir/tools/registry_maintenance.py" refresh \
    --binary "$binary" \
    --database "$database" \
    --runtime-dir "$runtime_dir" \
    --timezone "$local_timezone" \
    --backup-retention-count "$backup_retention_count" \
    -- \
    --request-timeout-seconds 60 \
    --stall-timeout-seconds 300 \
    --run-timeout-seconds 900 \
    --maximum-attempts-per-page 8 \
    --retry-initial-seconds 2 \
    --retry-maximum-seconds 120 \
    --maximum-pages 500 \
    --maximum-records 100000 \
    --maximum-page-bytes 8388608 \
    --verbose
