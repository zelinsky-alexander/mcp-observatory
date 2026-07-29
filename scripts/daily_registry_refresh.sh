#!/usr/bin/env bash

set -Eeuo pipefail
umask 077

PROJECT_DIR="$HOME/source/mcp-observatory"
BINARY="$PROJECT_DIR/build/release/mcp-observatory"
DATABASE="$PROJECT_DIR/db/local-registry.sqlite"

LOCAL_TIMEZONE="Asia/Jerusalem"

RUNTIME_DIR="$PROJECT_DIR/runtime/registry-refresh"
BUNDLE_DIR="$RUNTIME_DIR/bundles"
LOG_DIR="$RUNTIME_DIR/logs"
BACKUP_DIR="$RUNTIME_DIR/backups"

LOCK_FILE="$RUNTIME_DIR/refresh.lock"
LATEST_STATUS="$RUNTIME_DIR/latest-status.json"
LATEST_ERROR="$RUNTIME_DIR/latest-error.txt"

mkdir -p "$BUNDLE_DIR" "$LOG_DIR" "$BACKUP_DIR"

exec 9>"$LOCK_FILE"

if ! flock -n 9; then
    printf '%s\n' \
        "Another MCP Observatory refresh is already running." \
        >"$LATEST_ERROR"
    exit 20
fi

timestamp_utc="$(date -u +%Y%m%dT%H%M%SZ)"

started_at_utc="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
started_at_local="$(
    TZ="$LOCAL_TIMEZONE" date +%Y-%m-%dT%H:%M:%S%:z
)"

output="$BUNDLE_DIR/official-refresh-$timestamp_utc"
result="$LOG_DIR/official-refresh-$timestamp_utc.result.json"
progress="$LOG_DIR/official-refresh-$timestamp_utc.progress.log"
error_log="$LOG_DIR/official-refresh-$timestamp_utc.error.log"

backup_tmp="$BACKUP_DIR/local-registry.before-refresh.sqlite.tmp"
backup="$BACKUP_DIR/local-registry.before-refresh.sqlite"

cleanup_old_logs() {
    find "$LOG_DIR" \
        -type f \
        -mtime +30 \
        -delete \
        2>/dev/null || true
}

write_failure_status() {
    local exit_code="$1"
    local category="$2"
    local message="$3"

    local failed_at_utc
    local failed_at_local

    failed_at_utc="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    failed_at_local="$(
        TZ="$LOCAL_TIMEZONE" date +%Y-%m-%dT%H:%M:%S%:z
    )"

    printf '%s\n' "$message" >"$LATEST_ERROR"

    python3 - \
        "$LATEST_STATUS" \
        "$started_at_utc" \
        "$started_at_local" \
        "$failed_at_utc" \
        "$failed_at_local" \
        "$LOCAL_TIMEZONE" \
        "$exit_code" \
        "$category" \
        "$message" \
        "$output" \
        "$result" \
        "$progress" <<'PY'
import json
import sys
from pathlib import Path

(
    destination,
    started_at_utc,
    started_at_local,
    failed_at_utc,
    failed_at_local,
    timezone,
    exit_code,
    category,
    message,
    output,
    result,
    progress,
) = sys.argv[1:]

payload = {
    "status": "failed",
    "timezone": timezone,
    "started_at_utc": started_at_utc,
    "started_at_local": started_at_local,
    "failed_at_utc": failed_at_utc,
    "failed_at_local": failed_at_local,
    "exit_code": int(exit_code),
    "category": category,
    "message": message,
    "bundle": output,
    "result_log": result,
    "progress_log": progress,
}

path = Path(destination)
temporary = path.with_suffix(path.suffix + ".tmp")

temporary.write_text(
    json.dumps(payload, separators=(",", ":")) + "\n",
    encoding="utf-8",
)

temporary.replace(path)
PY
}

cd "$PROJECT_DIR"

cleanup_old_logs
rm -f "$LATEST_ERROR" "$backup_tmp"

if [[ ! -x "$BINARY" ]]; then
    write_failure_status \
        21 \
        binary_missing \
        "Release binary does not exist or is not executable: $BINARY"
    exit 21
fi

if [[ ! -f "$DATABASE" ]]; then
    write_failure_status \
        22 \
        database_missing \
        "Registry database does not exist: $DATABASE"
    exit 22
fi

integrity_before="$(
    sqlite3 "$DATABASE" 'PRAGMA integrity_check;'
)"

if [[ "$integrity_before" != "ok" ]]; then
    write_failure_status \
        23 \
        database_integrity_failed_before_refresh \
        "Pre-refresh SQLite integrity check failed: $integrity_before"
    exit 23
fi

foreign_keys_before="$(
    sqlite3 "$DATABASE" 'PRAGMA foreign_key_check;'
)"

if [[ -n "$foreign_keys_before" ]]; then
    printf '%s\n' "$foreign_keys_before" >"$error_log"

    write_failure_status \
        24 \
        database_foreign_key_failed_before_refresh \
        "Pre-refresh SQLite foreign-key validation failed."
    exit 24
fi

# Keep one transactionally consistent copy of the database as it existed
# immediately before the latest refresh.
sqlite3 "$DATABASE" ".backup '$backup_tmp'"
mv -f "$backup_tmp" "$backup"

set +e

"$BINARY" registry refresh \
    --database "$DATABASE" \
    --output "$output" \
    --format json \
    --request-timeout-seconds 60 \
    --stall-timeout-seconds 300 \
    --run-timeout-seconds 900 \
    --maximum-attempts-per-page 8 \
    --retry-initial-seconds 2 \
    --retry-maximum-seconds 120 \
    --maximum-pages 500 \
    --maximum-records 100000 \
    --maximum-page-bytes 8388608 \
    --verbose \
    >"$result" \
    2>"$progress"

refresh_exit=$?

set -e

if (( refresh_exit != 0 )); then
    tail -n 100 "$progress" \
        >"$error_log" \
        2>/dev/null || true

    write_failure_status \
        "$refresh_exit" \
        refresh_failed \
        "Registry refresh failed with exit code $refresh_exit. See $progress"

    exit "$refresh_exit"
fi

if [[ ! -f "$output/_SUCCESS" ]]; then
    write_failure_status \
        25 \
        success_marker_missing \
        "Refresh returned success but bundle _SUCCESS is missing."
    exit 25
fi

if ! "$BINARY" bundle validate "$output" \
    >>"$progress" \
    2>>"$error_log"; then

    write_failure_status \
        26 \
        bundle_validation_failed \
        "The generated incremental bundle failed validation: $output"

    exit 26
fi

integrity_after="$(
    sqlite3 "$DATABASE" 'PRAGMA integrity_check;'
)"

if [[ "$integrity_after" != "ok" ]]; then
    write_failure_status \
        27 \
        database_integrity_failed_after_refresh \
        "Post-refresh SQLite integrity check failed: $integrity_after"
    exit 27
fi

foreign_keys_after="$(
    sqlite3 "$DATABASE" 'PRAGMA foreign_key_check;'
)"

if [[ -n "$foreign_keys_after" ]]; then
    printf '%s\n' "$foreign_keys_after" >"$error_log"

    write_failure_status \
        28 \
        database_foreign_key_failed_after_refresh \
        "Post-refresh SQLite foreign-key validation failed."

    exit 28
fi

completed_at_utc="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
completed_at_local="$(
    TZ="$LOCAL_TIMEZONE" date +%Y-%m-%dT%H:%M:%S%:z
)"

python3 - \
    "$LATEST_STATUS" \
    "$started_at_utc" \
    "$started_at_local" \
    "$completed_at_utc" \
    "$completed_at_local" \
    "$LOCAL_TIMEZONE" \
    "$output" \
    "$result" \
    "$progress" <<'PY'
import json
import sys
from pathlib import Path

(
    destination,
    started_at_utc,
    started_at_local,
    completed_at_utc,
    completed_at_local,
    timezone,
    output,
    result_path,
    progress,
) = sys.argv[1:]

result_file = Path(result_path)
refresh_result = json.loads(
    result_file.read_text(encoding="utf-8")
)

payload = {
    "status": "completed",
    "timezone": timezone,
    "started_at_utc": started_at_utc,
    "started_at_local": started_at_local,
    "completed_at_utc": completed_at_utc,
    "completed_at_local": completed_at_local,
    "bundle": output,
    "result_log": result_path,
    "progress_log": progress,
    "refresh": refresh_result,
}

path = Path(destination)
temporary = path.with_suffix(path.suffix + ".tmp")

temporary.write_text(
    json.dumps(payload, separators=(",", ":")) + "\n",
    encoding="utf-8",
)

temporary.replace(path)
PY

rm -f "$LATEST_ERROR"
cleanup_old_logs

exit 0

