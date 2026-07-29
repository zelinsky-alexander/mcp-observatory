#!/usr/bin/env bash
set -euo pipefail

# Opt-in network integration check. Nothing invokes this script from CTest.
# Usage: tests/live_registry.sh ./build/dev-debug/mcp-observatory /tmp/mcpo-live-bundle

binary=${1:?mcp-observatory binary path required}
output=${2:?new output directory required}

"$binary" registry collect --output "$output" --verbose
"$binary" bundle validate "$output"
