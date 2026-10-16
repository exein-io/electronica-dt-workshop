#!/usr/bin/env bash
# Build runtime-lte (nuSIM overlay, precompiled monitor blob) into
# runtime-lte/build, then refuse an image that touches the nuSIM region.
# Extra arguments go to CMake, e.g. '-DCONFIG_RUNTIME_MONITOR_BOOTSTRAP_TOKEN="…"'.
set -euo pipefail
command -v west >/dev/null || . /opt/toolchain-env.sh   # Nordic toolchain image
export PROTOCOL_BUFFERS_PYTHON_IMPLEMENTATION=python

REPO=$(cd "$(dirname "$0")/.." && pwd)
WS=$(dirname "$REPO")
cd "$WS"
west build -b nrf9151dk/nrf9151/ns --sysbuild -p -d "$REPO/runtime-lte/build" "$REPO/runtime-lte" -- \
  -DEXTRA_CONF_FILE="$WS/modules/lib/redteamobile-nusim/overlay-nusim.conf" \
  -DZEPHYR_EXTRA_MODULES="$REPO/runtime-zephyr" "$@"
"$REPO/scripts/check-nusim-hex.py" "$REPO/runtime-lte/build/merged.hex"
echo "image: $REPO/runtime-lte/build/merged.hex"
