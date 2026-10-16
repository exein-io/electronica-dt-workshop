#!/usr/bin/env bash
# Serve the web flasher of a release on http://localhost:8765.
#
# The page, the wasm and the firmware come from the GitHub release (needs
# `gh`), the firmware optionally from HEX. Nothing is published:
# locally it listens on localhost only (a secure context, so WebUSB works); in
# a Codespace the forwarded port is private and behind your GitHub login.
#
#   webflash/serve.sh            # latest release
#   webflash/serve.sh v1.0.0     # a specific one
#   HEX=app/build/merged.hex webflash/serve.sh   # your own build
set -euo pipefail

REPO=exein-io/electronica-dt-workshop
TAG="${1:-$(gh release view -R "$REPO" --json tagName --jq .tagName)}"
DIR="$(mktemp -d)"
trap 'rm -rf "$DIR"' EXIT

gh release download "$TAG" -R "$REPO" -D "$DIR" -p index.html -p webflash.wasm -p workshop.hex
if [ -n "${HEX:-}" ]; then
  "$(dirname "$0")/../scripts/check-nusim-hex.py" "$HEX"
  cp "$HEX" "$DIR/workshop.hex"
  TAG="local build ($(basename "$HEX"))"
fi
printf '{"tag":"%s","file":"workshop.hex"}\n' "$TAG" > "$DIR/release.json"

echo "web flasher $TAG: http://localhost:8765  (Ctrl-C to stop)"
python3 -m http.server -d "$DIR" -b 127.0.0.1 8765
