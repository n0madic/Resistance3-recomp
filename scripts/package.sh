#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Pack a release: a zip with everything needed to play, and nothing derived from a game.
#
#   scripts/package.sh
#
# Writes dist/r3-<version>-macos-<arch>.zip, holding r3/ with the launcher, the prebuilt host, the SDK the launcher
# compiles the game against, and the licenses. The host is dist/host (scripts/build_host.sh builds it first when it is
# missing or older than its sources).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="$(cat "$ROOT/VERSION")"
cd "$ROOT"

[[ "$(uname -s)" == Darwin ]] || { echo "the host is built for macOS only" >&2; exit 1; }
ARCH="$(uname -m)"
NAME="r3-$VERSION-macos-$ARCH"
STAGE="$ROOT/dist/stage"

if [[ ! -f dist/host/r3_host || -n "$(find host patches scripts/build_host.sh scripts/lib_patches.sh scripts/make_host_header.py \
      -type f -newer dist/host/r3_host -print -quit)" ]]; then
  "$ROOT/scripts/build_host.sh"
fi

rm -rf "${STAGE:?}"
mkdir -p "$STAGE/r3"
cp README.md CLAUDE.md LICENSE LICENSE-GPL-2.0.txt VERSION r3.py "$STAGE/r3/"   # CLAUDE.md: the README links to it
cp -R launcher "$STAGE/r3/launcher"
cp -R docs "$STAGE/r3/docs"
cp -R dist/host "$STAGE/r3/host"
mv "$STAGE/r3/host/licenses" "$STAGE/r3/licenses"
find "$STAGE" \( -name __pycache__ -o -name .DS_Store \) -prune -exec rm -rf {} +

rm -f "$ROOT/dist/$NAME.zip"
(cd "$STAGE" && zip -qr -X "$ROOT/dist/$NAME.zip" r3)
rm -rf "${STAGE:?}"
echo "packed: dist/$NAME.zip ($(du -h "$ROOT/dist/$NAME.zip" | cut -f1))"
