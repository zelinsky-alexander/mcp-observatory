#!/usr/bin/env bash

set -Eeuo pipefail
umask 027

service_name="mcp-observatory-static-analysis.service"
timer_name="mcp-observatory-static-analysis.timer"
refresh_service_name="mcp-observatory-refresh.service"

install_root="${MCPO_INSTALL_ROOT:-/opt/mcp-observatory/current}"
state_root="${MCPO_STATE_ROOT:-/var/lib/mcp-observatory}"
database="${MCPO_DATABASE:-$state_root/catalog/local-registry.sqlite}"
evidence_root="${MCPO_EVIDENCE_ROOT:-$state_root/evidence}"
service_user="${MCPO_STATIC_ANALYSIS_USER:-mcp-refresh}"
service_group="${MCPO_STATIC_ANALYSIS_GROUP:-mcp-refresh}"
catalog_group="${MCPO_CATALOG_GROUP:-mcp-catalog}"
systemd_dir="${MCPO_SYSTEMD_DIR:-/etc/systemd/system}"
config_dir="${MCPO_CONFIG_DIR:-/etc/mcp-observatory}"

batch_size="${MCPO_STATIC_ANALYSIS_BATCH_SIZE:-1000}"
maximum_run_seconds="${MCPO_STATIC_ANALYSIS_MAXIMUM_RUN_SECONDS:-3000}"
child_timeout_seconds="${MCPO_STATIC_ANALYSIS_CHILD_TIMEOUT_SECONDS:-300}"
maximum_attempts="${MCPO_STATIC_ANALYSIS_MAXIMUM_ATTEMPTS:-3}"
retry_failed_after_seconds="${MCPO_STATIC_ANALYSIS_RETRY_FAILED_AFTER_SECONDS:-86400}"
stale_running_after_seconds="${MCPO_STATIC_ANALYSIS_STALE_RUNNING_AFTER_SECONDS:-3600}"

start_now=1
accept_docker_privilege=0

log() {
    printf '[install-static-analysis-scheduler] %s\n' "$*" >&2
}

fail() {
    log "error: $*"
    exit 1
}

usage() {
    cat <<'EOF'
Usage:
  sudo bash scripts/install_static_analysis_scheduler.sh \
    --accept-docker-root-equivalent [--no-start]

Installs and enables the production systemd service and timer for bounded static
artifact analysis. It also installs a refresh-service drop-in so every successful
catalog publication requests an analysis run.

Options:
  --accept-docker-root-equivalent
      Required acknowledgement. Access to the host Docker daemon is effectively
      root-equivalent. The public portal account is not granted this access.
  --no-start
      Install and enable the timer without requesting an immediate analysis run.
  --help
      Show this help.

Environment overrides:
  MCPO_INSTALL_ROOT, MCPO_STATE_ROOT, MCPO_DATABASE, MCPO_EVIDENCE_ROOT,
  MCPO_STATIC_ANALYSIS_USER, MCPO_STATIC_ANALYSIS_GROUP, MCPO_CATALOG_GROUP,
  MCPO_SYSTEMD_DIR, MCPO_CONFIG_DIR, MCPO_STATIC_ANALYSIS_BATCH_SIZE,
  MCPO_STATIC_ANALYSIS_MAXIMUM_RUN_SECONDS,
  MCPO_STATIC_ANALYSIS_CHILD_TIMEOUT_SECONDS,
  MCPO_STATIC_ANALYSIS_MAXIMUM_ATTEMPTS,
  MCPO_STATIC_ANALYSIS_RETRY_FAILED_AFTER_SECONDS,
  MCPO_STATIC_ANALYSIS_STALE_RUNNING_AFTER_SECONDS.
EOF
}

require_command() {
    command -v "$1" >/dev/null 2>&1 || fail "required command is missing: $1"
}

require_positive_integer() {
    local name="$1"
    local value="$2"
    [[ "$value" =~ ^[1-9][0-9]*$ ]] || fail "$name must be a positive integer"
}

require_non_negative_integer() {
    local name="$1"
    local value="$2"
    [[ "$value" =~ ^[0-9]+$ ]] || fail "$name must be a non-negative integer"
}

require_safe_unit_value() {
    local name="$1"
    local value="$2"
    [[ "$value" =~ ^[A-Za-z0-9_./:@+-]+$ ]] \
        || fail "$name contains characters unsafe for generated unit files"
}

parse_arguments() {
    while (($# > 0)); do
        case "$1" in
            --accept-docker-root-equivalent)
                accept_docker_privilege=1
                ;;
            --no-start)
                start_now=0
                ;;
            --help|-h)
                usage
                exit 0
                ;;
            *)
                fail "unknown argument: $1"
                ;;
        esac
        shift
    done
}

validate_host() {
    [[ ${EUID:-$(id -u)} -eq 0 ]] || fail "run this installer as root"
    (( accept_docker_privilege == 1 )) \
        || fail "pass --accept-docker-root-equivalent after reviewing the Docker privilege boundary"

    for command_name in \
        systemctl systemd-analyze install getent id runuser flock docker stat
    do
        require_command "$command_name"
    done

    id "$service_user" >/dev/null 2>&1 \
        || fail "service user does not exist: $service_user"
    getent group "$service_group" >/dev/null \
        || fail "service group does not exist: $service_group"
    getent group "$catalog_group" >/dev/null \
        || fail "catalog group does not exist: $catalog_group"
    getent group docker >/dev/null \
        || fail "docker group does not exist; configure the Docker daemon first"

    [[ -d "$install_root" ]] || fail "release/current directory is missing: $install_root"
    [[ -x "$install_root/build/release/mcp-observatory" ]] \
        || fail "release analyzer binary is not executable"
    [[ -f "$install_root/tools/bulk_static_analysis.py" ]] \
        || fail "bulk scheduler is missing from the deployed release"
    [[ -f "$install_root/scripts/run_static_analysis_queue.sh" ]] \
        || fail "static-analysis queue wrapper is missing from the deployed release"
    [[ -f "$install_root/rules/artifact-static-analysis-v1.json" ]] \
        || fail "static-analysis rules are missing from the deployed release"
    [[ -f "$database" ]] || fail "published catalog database is missing: $database"

    runuser -u "$service_user" -- test -w "$database" \
        || fail "$service_user cannot write the catalog database"
    runuser -u "$service_user" -- test -w "$(dirname -- "$database")" \
        || fail "$service_user cannot create the catalog writer lock"

    require_positive_integer MCPO_STATIC_ANALYSIS_BATCH_SIZE "$batch_size"
    require_positive_integer MCPO_STATIC_ANALYSIS_MAXIMUM_RUN_SECONDS "$maximum_run_seconds"
    require_positive_integer MCPO_STATIC_ANALYSIS_CHILD_TIMEOUT_SECONDS "$child_timeout_seconds"
    require_positive_integer MCPO_STATIC_ANALYSIS_MAXIMUM_ATTEMPTS "$maximum_attempts"
    require_non_negative_integer \
        MCPO_STATIC_ANALYSIS_RETRY_FAILED_AFTER_SECONDS \
        "$retry_failed_after_seconds"
    require_non_negative_integer \
        MCPO_STATIC_ANALYSIS_STALE_RUNNING_AFTER_SECONDS \
        "$stale_running_after_seconds"
    (( maximum_run_seconds > child_timeout_seconds )) \
        || fail "maximum scheduler duration must exceed the per-package timeout"

    for pair in \
        "MCPO_INSTALL_ROOT:$install_root" \
        "MCPO_STATE_ROOT:$state_root" \
        "MCPO_DATABASE:$database" \
        "MCPO_EVIDENCE_ROOT:$evidence_root" \
        "MCPO_STATIC_ANALYSIS_USER:$service_user" \
        "MCPO_STATIC_ANALYSIS_GROUP:$service_group" \
        "MCPO_CATALOG_GROUP:$catalog_group" \
        "MCPO_SYSTEMD_DIR:$systemd_dir" \
        "MCPO_CONFIG_DIR:$config_dir"
    do
        require_safe_unit_value "${pair%%:*}" "${pair#*:}"
    done
}

write_configuration() {
    install -d -o root -g root -m 0755 "$config_dir"
    local temporary
    temporary="$(mktemp "$config_dir/.static-analysis.env.XXXXXX")"
    trap 'rm -f -- "${temporary:-}"' RETURN
    cat >"$temporary" <<EOF
MCPO_PROJECT_DIR=$install_root
MCPO_BINARY=$install_root/build/release/mcp-observatory
MCPO_DATABASE=$database
MCPO_EVIDENCE_ROOT=$evidence_root
MCPO_STATIC_ANALYSIS_RULES=$install_root/rules/artifact-static-analysis-v1.json
MCPO_STATIC_ANALYSIS_BATCH_SIZE=$batch_size
MCPO_STATIC_ANALYSIS_MAXIMUM_RUN_SECONDS=$maximum_run_seconds
MCPO_STATIC_ANALYSIS_CHILD_TIMEOUT_SECONDS=$child_timeout_seconds
MCPO_STATIC_ANALYSIS_MAXIMUM_ATTEMPTS=$maximum_attempts
MCPO_STATIC_ANALYSIS_RETRY_FAILED_AFTER_SECONDS=$retry_failed_after_seconds
MCPO_STATIC_ANALYSIS_STALE_RUNNING_AFTER_SECONDS=$stale_running_after_seconds
EOF
    install -o root -g root -m 0640 \
        "$temporary" "$config_dir/static-analysis.env"
    rm -f -- "$temporary"
    trap - RETURN
}

write_units() {
    install -d -o root -g root -m 0755 "$systemd_dir"

    local service_tmp timer_tmp dropin_dir dropin_tmp
    service_tmp="$(mktemp)"
    timer_tmp="$(mktemp)"
    dropin_dir="$systemd_dir/$refresh_service_name.d"
    dropin_tmp="$(mktemp)"
    trap 'rm -f -- "${service_tmp:-}" "${timer_tmp:-}" "${dropin_tmp:-}"' RETURN

    cat >"$service_tmp" <<EOF
[Unit]
Description=MCP Observatory bounded static artifact analysis
Documentation=file:$install_root/docs/bulk-static-analysis.md
Wants=network-online.target
After=network-online.target $refresh_service_name

[Service]
Type=oneshot
User=$service_user
Group=$service_group
SupplementaryGroups=$catalog_group docker
WorkingDirectory=$install_root
EnvironmentFile=$config_dir/static-analysis.env
ExecStart=/bin/bash $install_root/scripts/run_static_analysis_queue.sh
UMask=0027

NoNewPrivileges=yes
PrivateDevices=yes
PrivateTmp=yes
ProtectClock=yes
ProtectControlGroups=yes
ProtectHome=yes
ProtectHostname=yes
ProtectKernelLogs=yes
ProtectKernelModules=yes
ProtectKernelTunables=yes
ProtectSystem=strict
ReadWritePaths=$state_root
RestrictAddressFamilies=AF_UNIX AF_INET AF_INET6
RestrictNamespaces=yes
RestrictRealtime=yes
RestrictSUIDSGID=yes
LockPersonality=yes
MemoryDenyWriteExecute=yes
CapabilityBoundingSet=
AmbientCapabilities=
IPAddressDeny=link-local

TimeoutStartSec=$((maximum_run_seconds + 300))
KillMode=control-group
KillSignal=SIGTERM
SendSIGKILL=yes
EOF

    cat >"$timer_tmp" <<EOF
[Unit]
Description=Continue MCP Observatory static artifact backfill hourly

[Timer]
OnBootSec=10min
OnCalendar=hourly
RandomizedDelaySec=5min
AccuracySec=1min
Persistent=yes
Unit=$service_name

[Install]
WantedBy=timers.target
EOF

    cat >"$dropin_tmp" <<EOF
[Unit]
OnSuccess=$service_name
EOF

    install -o root -g root -m 0644 "$service_tmp" "$systemd_dir/$service_name"
    install -o root -g root -m 0644 "$timer_tmp" "$systemd_dir/$timer_name"
    install -d -o root -g root -m 0755 "$dropin_dir"
    install -o root -g root -m 0644 \
        "$dropin_tmp" "$dropin_dir/static-analysis.conf"

    rm -f -- "$service_tmp" "$timer_tmp" "$dropin_tmp"
    trap - RETURN
}

prepare_state() {
    install -d -o "$service_user" -g "$catalog_group" -m 0750 "$evidence_root"
}

activate_units() {
    systemd-analyze verify \
        "$systemd_dir/$service_name" \
        "$systemd_dir/$timer_name"
    systemctl daemon-reload
    systemctl enable --now "$timer_name"
    if (( start_now == 1 )); then
        systemctl start --no-block "$service_name"
    fi
}

show_result() {
    log "installed $systemd_dir/$service_name"
    log "installed $systemd_dir/$timer_name"
    log "installed $systemd_dir/$refresh_service_name.d/static-analysis.conf"
    log "installed $config_dir/static-analysis.env"
    log "evidence directory: $evidence_root"
    log "timer status: $(systemctl is-enabled "$timer_name") / $(systemctl is-active "$timer_name")"
    if (( start_now == 1 )); then
        log "initial analysis run requested asynchronously"
    fi
    log "inspect with: systemctl status $service_name $timer_name"
    log "follow logs with: journalctl -u $service_name -f"
}

main() {
    parse_arguments "$@"
    validate_host

    install -d -o root -g root -m 0755 /run/lock
    exec 9>/run/lock/mcp-observatory-static-analysis-install.lock
    flock 9

    prepare_state
    write_configuration
    write_units
    activate_units
    show_result
}

main "$@"
