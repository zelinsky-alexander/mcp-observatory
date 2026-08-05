#!/usr/bin/env bash

set -Eeuo pipefail
umask 027

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
project_dir="${MCPO_PROJECT_DIR:-$(dirname -- "$script_dir")}" 
binary="${MCPO_BINARY:-$project_dir/build/release/mcp-observatory}"
database="${MCPO_DATABASE:-$project_dir/db/local-registry.sqlite}"
rules="${MCPO_STATIC_ANALYSIS_RULES:-$project_dir/rules/artifact-static-analysis-v1.json}"
evidence_root="${MCPO_EVIDENCE_ROOT:-$project_dir/evidence}"
batch_size="${MCPO_STATIC_ANALYSIS_BATCH_SIZE:-1000}"
maximum_run_seconds="${MCPO_STATIC_ANALYSIS_MAXIMUM_RUN_SECONDS:-3000}"
child_timeout_seconds="${MCPO_STATIC_ANALYSIS_CHILD_TIMEOUT_SECONDS:-300}"
maximum_attempts="${MCPO_STATIC_ANALYSIS_MAXIMUM_ATTEMPTS:-3}"
retry_failed_after_seconds="${MCPO_STATIC_ANALYSIS_RETRY_FAILED_AFTER_SECONDS:-86400}"
stale_running_after_seconds="${MCPO_STATIC_ANALYSIS_STALE_RUNNING_AFTER_SECONDS:-3600}"

log() {
    printf '[static-analysis-queue] %s\n' "$*" >&2
}

fail() {
    log "error: $*"
    exit 1
}

require_positive_integer() {
    local name="$1"
    local value="$2"
    [[ "$value" =~ ^[1-9][0-9]*$ ]] || fail "$name must be a positive integer"
}

validate_configuration() {
    [[ -x "$binary" ]] || fail "Observatory binary is not executable: $binary"
    [[ -f "$database" ]] || fail "catalog database does not exist: $database"
    [[ -f "$rules" ]] || fail "analysis rules do not exist: $rules"
    [[ -f "$project_dir/tools/bulk_static_analysis.py" ]] \
        || fail "bulk static-analysis scheduler is missing"
    mkdir -p -- "$evidence_root"

    require_positive_integer MCPO_STATIC_ANALYSIS_BATCH_SIZE "$batch_size"
    require_positive_integer MCPO_STATIC_ANALYSIS_MAXIMUM_RUN_SECONDS "$maximum_run_seconds"
    require_positive_integer MCPO_STATIC_ANALYSIS_CHILD_TIMEOUT_SECONDS "$child_timeout_seconds"
    require_positive_integer MCPO_STATIC_ANALYSIS_MAXIMUM_ATTEMPTS "$maximum_attempts"
    [[ "$retry_failed_after_seconds" =~ ^[0-9]+$ ]] \
        || fail "MCPO_STATIC_ANALYSIS_RETRY_FAILED_AFTER_SECONDS must be non-negative"
    [[ "$stale_running_after_seconds" =~ ^[0-9]+$ ]] \
        || fail "MCPO_STATIC_ANALYSIS_STALE_RUNNING_AFTER_SECONDS must be non-negative"
    (( maximum_run_seconds > child_timeout_seconds )) \
        || fail "maximum run duration must exceed the child timeout"
}

main() {
    validate_configuration
    log "starting bounded static-analysis maintenance"
    log "database=$database"
    log "maximum_run_seconds=$maximum_run_seconds batch_size=$batch_size"

    python3 "$project_dir/tools/bulk_static_analysis.py" \
        --database "$database" \
        --observatory-binary "$binary" \
        --rules "$rules" \
        --evidence-root "$evidence_root" \
        --batch-size "$batch_size" \
        --maximum-run-seconds "$maximum_run_seconds" \
        --child-timeout-seconds "$child_timeout_seconds" \
        --maximum-attempts "$maximum_attempts" \
        --retry-failed-after-seconds "$retry_failed_after_seconds" \
        --stale-running-after-seconds "$stale_running_after_seconds" \
        --format json

    log "bounded static-analysis maintenance completed"
}

main "$@"
