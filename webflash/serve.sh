#!/usr/bin/env bash
# Serve the web flasher of a release on http://localhost:8765.
#
# The page, the wasm and the firmware come from the GitHub release, the
# firmware optionally from HEX. Locally it listens on localhost only (a
# secure context, so WebUSB works); in a Codespace the forwarded port is
# private and behind your GitHub login.
#
#   webflash/serve.sh                                     # latest release
#   webflash/serve.sh v1.0.0                              # a specific one
#   HEX=app/build/merged.hex webflash/serve.sh            # your own build
set -euo pipefail

REPO=exein-io/electronica-dt-workshop
TAG="${1:-$(curl -fsSL "https://api.github.com/repos/$REPO/releases/latest" | python3 -c 'import json,sys; print(json.load(sys.stdin)["tag_name"])')}"
DIR="$(mktemp -d)"
trap 'rm -rf "$DIR"' EXIT

for f in index.html webflash.wasm workshop.hex; do
  curl -fsSL -o "$DIR/$f" "https://github.com/$REPO/releases/download/$TAG/$f"
done
if [ -n "${HEX:-}" ]; then
  "$(dirname "$0")/../scripts/check-nusim-hex.py" "$HEX"
  cp "$HEX" "$DIR/workshop.hex"
  TAG="local build ($(basename "$HEX"))"
fi
printf '{"tag":"%s","file":"workshop.hex"}\n' "$TAG" > "$DIR/release.json"

echo "web flasher $TAG: http://localhost:8765  (Ctrl-C to stop)"
python3 -m http.server -d "$DIR" -b 127.0.0.1 8765
