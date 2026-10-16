#!/usr/bin/env bash
# Set up the west workspace in the parent folder of this repo, with the
# vendored ncs-nusim-app snapshot as manifest. Idempotent: a second run only
# updates. Used by the devcontainer and CI.
set -euo pipefail
command -v west >/dev/null || . /opt/toolchain-env.sh   # Nordic toolchain image

REPO=$(cd "$(dirname "$0")/.." && pwd)
cd "$(dirname "$REPO")"
NUSIM=modules/lib/redteamobile-nusim

git config --global --add safe.directory '*'
if [ ! -d .west ]; then
  mkdir -p modules/lib
  python3 -m zipfile -e "$REPO"/vendor/ncs-nusim-app-ncs-v3.1.0-*.zip modules/lib
  git -C $NUSIM init -q
  git -C $NUSIM add -A
  git -C $NUSIM -c user.name=setup -c user.email=setup@localhost commit -qm "vendored ncs-nusim-app"
  # west init -l would make modules/lib the topdir; pin it here.
  mkdir .west
  printf '[manifest]\npath = %s\nfile = west.yml\n' $NUSIM > .west/config
fi
west update --narrow -o=--depth=1
