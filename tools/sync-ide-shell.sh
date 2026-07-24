#!/bin/sh
# Vendor the IDE loader shell into this firmware.
#
# shell.html is CANONICAL in sprocket-robotics/ide (it lives beside the app
# whose CORS/markup contract it depends on — see the header comment in the
# file itself). The hub role serves the vendored copy at /ide/: a ~2 KB stub
# that fetches the full editor from the ide repo's GitHub Pages deploy at
# runtime, so the 4 MB firmware never carries the 619 KB bundle again. Kept
# byte-identical to canonical so drift is a plain diff.
#
# Run after a shell change in ide:      tools/sync-ide-shell.sh
# CI / pre-flash freshness gate:        tools/sync-ide-shell.sh --check
# Point at an ide checkout elsewhere:   IDE_REPO=/path/to/ide tools/sync-ide-shell.sh
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
src="${IDE_REPO:-$here/../../ide}/shell.html"
dst="$here/../web/ide_shell.html"

if [ ! -f "$src" ]; then
  echo "canonical shell not found: $src" >&2
  echo "  clone sprocket-robotics/ide as a sibling, or set IDE_REPO=path/to/ide" >&2
  exit 2
fi

if [ "${1:-}" = "--check" ]; then
  if cmp -s "$src" "$dst"; then
    echo "ide shell in sync with canonical"
  else
    echo "DRIFT: $dst differs from $src — run tools/sync-ide-shell.sh" >&2
    exit 1
  fi
else
  cp "$src" "$dst"
  echo "synced web/ide_shell.html <- $src"
fi
