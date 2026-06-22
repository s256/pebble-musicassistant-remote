#!/usr/bin/env bash
#
# scripts/make-publish-pbw.sh
#
# Produce a clean .pbw fit for upload to the Rebble appstore.
#
# Pebble's bundle step embeds a JS source map (pebble-js-app.js.map) which
# contains the local SDK install path of whoever built the artifact.  That
# path leaks the build machine's username.  The map file is not loaded at
# runtime — it only matters when debugging JS — so we drop it from the
# uploadable copy.
#
# Output:
#   build/pebble-musicassistant-publish.pbw    <- upload this
#
# The original build/pebble-musicassistant.pbw is left untouched for local
# install / emulator use (the map helps when reading PebbleKit JS logs).
#
set -euo pipefail

cd "$(dirname "$0")/.."

SRC=build/pebble-musicassistant.pbw
DST=build/pebble-musicassistant-publish.pbw

if [[ ! -f "$SRC" ]]; then
  echo "error: $SRC not found - run 'pebble build' first" >&2
  exit 1
fi

cp -f "$SRC" "$DST"

if unzip -l "$DST" | grep -q 'pebble-js-app.js.map'; then
  zip -dq "$DST" pebble-js-app.js.map
fi

echo "Wrote $DST"
unzip -l "$DST"

# Verify no '/home/' personal-path leaks remain.  Loud failure so a future
# maintainer notices if waf starts inlining build paths somewhere new.
LEAKS=$(unzip -p "$DST" pebble-js-app.js 2>/dev/null | grep -c '/home/' || true)
if [[ "$LEAKS" -gt 0 ]]; then
  echo "WARNING: $LEAKS personal-path string(s) still in the bundled JS" >&2
fi
