#!/bin/sh
# Vendor the block IDE into this firmware.
#
# The IDE is CANONICAL in better-robotics/ide; the hub role serves its ESP32
# bundle (Blockly + textarea editor, no Monaco — ~400 KB gzipped) at /ide/.
# Unlike dashboard.html (synced from a sibling checkout), the bundle only
# exists BUILT — it carries ide's vendored Blockly/mqtt.js, which aren't in
# its git tree — so this syncs from ide's release asset, pinned by tag.
# Bumping IDE_TAG is a deliberate edit + commit here, never a build side
# effect (same discipline as hub's BASE_* image pins).
#
# Run after an ide release:            tools/sync-ide.sh
# Pre-flash freshness gate:            tools/sync-ide.sh --check
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
dst="$here/../web/ide"

IDE_TAG=ide-v7

if [ "${1:-}" = "--check" ]; then
  if [ -f "$dst/.tag" ] && [ "$(cat "$dst/.tag")" = "$IDE_TAG" ]; then
    echo "web/ide in sync ($IDE_TAG)"
  else
    echo "DRIFT: web/ide is $(cat "$dst/.tag" 2>/dev/null || echo missing), pinned $IDE_TAG — run tools/sync-ide.sh" >&2
    exit 1
  fi
  exit 0
fi

url="https://github.com/better-robotics/ide/releases/download/$IDE_TAG/ide-esp32-dist.tar.gz"
tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT
curl -fsSL "$url" -o "$tmp"
rm -rf "$dst"
mkdir -p "$dst"
tar -xzf "$tmp" -C "$dst"
printf '%s' "$IDE_TAG" > "$dst/.tag"
echo "synced web/ide <- $url"
