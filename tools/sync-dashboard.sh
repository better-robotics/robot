#!/bin/sh
# Vendor the classroom dashboard into this firmware.
#
# dashboard.html is CANONICAL in sprocket-robotics/hub (the monorepo shares one
# copy across pi/ and the ESP hub). This repo embeds a synced copy so the hub
# role can serve the full UI offline (EMBED_TXTFILES, src/CMakeLists.txt). The
# vendored copy is kept byte-identical to canonical so drift is a plain diff.
#
# Run after any dashboard change in hub:   tools/sync-dashboard.sh
# CI / pre-flash freshness gate:           tools/sync-dashboard.sh --check
# Point at a hub checkout elsewhere:        HUB_REPO=/path/to/hub tools/sync-dashboard.sh
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
src="${HUB_REPO:-$here/../../hub}/dashboard.html"
dst="$here/../web/dashboard.html"

if [ ! -f "$src" ]; then
  echo "canonical dashboard not found: $src" >&2
  echo "  clone sprocket-robotics/hub as a sibling, or set HUB_REPO=path/to/hub" >&2
  exit 2
fi

if [ "${1:-}" = "--check" ]; then
  if cmp -s "$src" "$dst"; then
    echo "dashboard in sync with canonical"
  else
    echo "DRIFT: $dst differs from $src — run tools/sync-dashboard.sh" >&2
    exit 1
  fi
else
  cp "$src" "$dst"
  echo "synced web/dashboard.html <- $src"
fi
