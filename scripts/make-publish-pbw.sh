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

# --- Version-stamp sanity check ----------------------------------------
# waf aggressively caches appinfo.json from a previous build, so bumping
# package.json without `pebble clean` leaves the OLD version inside the
# .pbw.  This bit me with v1.0.1, which shipped a binary still stamped
# 1.0.0.  Refuse to produce a publish artifact unless the embedded
# versionLabel matches package.json.

PKG_VERSION=$(python3 -c "import json; print(json.load(open('package.json'))['version'])")
PBW_VERSION=$(unzip -p "$SRC" appinfo.json | python3 -c "import json, sys; print(json.load(sys.stdin).get('versionLabel', ''))")

if [[ "$PKG_VERSION" != "$PBW_VERSION" ]]; then
  echo "error: package.json version ($PKG_VERSION) does not match .pbw appinfo.json versionLabel ($PBW_VERSION)" >&2
  echo "       waf likely cached an older appinfo.  Run:" >&2
  echo "         pebble clean && pebble build" >&2
  echo "       then re-run this script." >&2
  exit 1
fi

cp -f "$SRC" "$DST"

if unzip -l "$DST" | grep -q 'pebble-js-app.js.map'; then
  zip -dq "$DST" pebble-js-app.js.map
fi

echo "Wrote $DST  (version $PBW_VERSION)"
unzip -l "$DST"

# Verify no '/home/' personal-path leaks remain.  Loud failure so a future
# maintainer notices if waf starts inlining build paths somewhere new.
LEAKS=$(unzip -p "$DST" pebble-js-app.js 2>/dev/null | grep -c '/home/' || true)
if [[ "$LEAKS" -gt 0 ]]; then
  echo "WARNING: $LEAKS personal-path string(s) still in the bundled JS" >&2
fi
