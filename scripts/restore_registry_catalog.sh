#!/usr/bin/env bash

set -Eeuo pipefail
umask 077

if (( $# != 2 )); then
    printf 'usage: %s DATABASE BACKUP\n' "$0" >&2
    exit 2
fi

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
exec python3 "$script_dir/../tools/registry_maintenance.py" restore \
    --database "$1" --backup "$2"
