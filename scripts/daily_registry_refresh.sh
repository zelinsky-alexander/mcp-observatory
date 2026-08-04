#!/usr/bin/env bash

set -Eeuo pipefail

# Files created by this wrapper should be private by default while still
# allowing explicitly assigned group-read access where required.
umask 027

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
project_dir="$(dirname -- "$script_dir")"

project_dir="${MCPO_PROJECT_DIR:-$project_dir}"
binary="${MCPO_BINARY:-$project_dir/build/release/mcp-observatory}"
database="${MCPO_DATABASE:-$project_dir/db/local-registry.sqlite}"
runtime_dir="${MCPO_REFRESH_RUNTIME_DIR:-$project_dir/runtime/registry-refresh}"
local_timezone="${MCPO_LOCAL_TIMEZONE:-UTC}"
backup_retention_count="${MCPO_BACKUP_RETENTION_COUNT:-2}"
catalog_group="${MCPO_CATALOG_GROUP:-mcp-catalog}"

log() {
    printf '[daily-registry-refresh] %s\n' "$*" >&2
}

fail() {
    log "error: $*"
    exit 1
}

publish_catalog_permissions() {
    local database_path="${1:?database path required}"
    local catalog_directory
    local state_directory

    catalog_directory="$(dirname -- "$database_path")"
    state_directory="$(dirname -- "$catalog_directory")"

    [[ -f "$database_path" ]] \
        || fail "published database does not exist: $database_path"

    [[ -d "$catalog_directory" ]] \
        || fail "catalog directory does not exist: $catalog_directory"

    [[ -d "$state_directory" ]] \
        || fail "state directory does not exist: $state_directory"

    # The refresh account must own these paths or otherwise have permission
    # to assign their group to the configured shared catalog group.
    chgrp -- "$catalog_group" \
        "$state_directory" \
        "$catalog_directory" \
        "$database_path"

    chmod -- 0750 \
        "$state_directory" \
        "$catalog_directory"

    chmod -- 0640 \
        "$database_path"

    log "published catalog permissions:"
    stat -c '%A %a %U:%G %n' \
        "$state_directory" \
        "$catalog_directory" \
        "$database_path" >&2
}

validate_configuration() {
    [[ -d "$project_dir" ]] \
        || fail "project directory does not exist: $project_dir"

    [[ -x "$binary" ]] \
        || fail "Observatory binary is not executable: $binary"

    [[ -f "$project_dir/tools/registry_maintenance.py" ]] \
        || fail "registry maintenance tool is missing"

    [[ "$backup_retention_count" =~ ^[0-9]+$ ]] \
        || fail "MCPO_BACKUP_RETENTION_COUNT must be a non-negative integer"

    getent group "$catalog_group" >/dev/null \
        || fail "catalog group does not exist: $catalog_group"
}

main() {
    validate_configuration

    log "starting registry refresh"
    log "database=$database"
    log "runtime_dir=$runtime_dir"

    # registry_maintenance.py performs staging, validation, backup, and atomic
    # publication. With `set -e`, a non-zero result terminates this script and
    # the permission publication below is not executed.
    python3 "$project_dir/tools/registry_maintenance.py" refresh \
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

    # Reached only after registry_maintenance.py reports successful publication.
    publish_catalog_permissions "$database"

    log "registry refresh and catalog publication completed successfully"
}

main "$@"

