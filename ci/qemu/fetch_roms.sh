#!/bin/bash
# Fetch one camera's ROM dumps from the private Forgejo generic package into DEST/CAM/.
# Runs on the CI host (outside containers). See docs/EMULATION.md "ROM handling".
#
# Usage: ci/qemu/fetch_roms.sh CAM DEST
# Package: $FORGEJO_URL/api/packages/$ROM_PKG_OWNER/generic/canon-rom/<CAM>-<fw>/{ROM0.BIN,ROM1.BIN,SFDATA.BIN}
#          (<fw> from platform/CAM.<fw>, e.g. EOSM-202, 1100D-105)
# Auth:    $ROM_TOKEN (repo secret: a personal access token with only read:package), else $JOB_TOKEN.
# Exit:    0 fetched and sha256-verified, 3 not available (no token / no package / no access), 1 error.
# The token is passed to curl through a mode-600 header file, never on a command line or in the log.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
CAM=${1:?usage: fetch_roms.sh CAM DEST}
DEST=${2:?usage: fetch_roms.sh CAM DEST}
FORGEJO_URL=${FORGEJO_URL:-${GITHUB_SERVER_URL:-<forgejo-server>}}
OWNER=${ROM_PKG_OWNER:-<private-org>}
PKG=canon-rom

case "$CAM" in *[!A-Za-z0-9]*|"") echo "bad CAM '$CAM'" >&2; exit 1;; esac
plat=$(cd "$ROOT/platform" && ls -d "$CAM".[0-9]* 2>/dev/null | head -1)
[ -n "$plat" ] || { echo "no platform/$CAM.<fw>" >&2; exit 1; }
VERSION=$CAM-${plat#*.}

token=${ROM_TOKEN:-${JOB_TOKEN:-}}
if [ -z "$token" ]; then
    echo "ROMs for $CAM: unavailable (no ROM_TOKEN secret and no job token)"
    exit 3
fi

umask 077
mkdir -p "$DEST/$CAM"
hdr=$(mktemp "$DEST/.hdr.XXXXXX")
trap 'rm -f "$hdr"' EXIT
printf 'Authorization: token %s\n' "$token" > "$hdr"
unset token ROM_TOKEN JOB_TOKEN

api="$FORGEJO_URL/api/v1/packages/$OWNER/generic/$PKG/$VERSION/files"
list=$(mktemp "$DEST/.list.XXXXXX")
code=$(curl -s -o "$list" -w '%{http_code}' -H "@$hdr" "$api" || echo 000)
if [ "$code" != 200 ]; then
    rm -f "$list"
    echo "ROMs for $CAM: unavailable (package $OWNER/$PKG/$VERSION: HTTP $code)"
    exit 3
fi

# name<TAB>sha256 for the files we know; anything else in the package is ignored.
mapfile -t files < <(python3 -I -c '
import json, sys
for f in json.load(open(sys.argv[1])):
    if f["name"] in ("ROM0.BIN", "ROM1.BIN", "SFDATA.BIN"):
        print(f["name"] + "\t" + f["sha256"])
' "$list")
rm -f "$list"
[ "${#files[@]}" -gt 0 ] || { echo "ROMs for $CAM: unavailable (package has no ROM files)"; exit 3; }

for entry in "${files[@]}"; do
    name=${entry%%$'\t'*}; sha=${entry#*$'\t'}
    curl -sf -H "@$hdr" -o "$DEST/$CAM/$name" "$FORGEJO_URL/api/packages/$OWNER/generic/$PKG/$VERSION/$name" \
        || { echo "download of $name failed" >&2; exit 1; }
    echo "$sha  $DEST/$CAM/$name" | sha256sum -c --quiet - \
        || { echo "sha256 mismatch for $name" >&2; rm -f "$DEST/$CAM/$name"; exit 1; }
    echo "ROMs for $CAM: $name ok ($(stat -c %s "$DEST/$CAM/$name") bytes, sha256 verified)"
done
"$ROOT/ci/qemu/check_rom_md5.sh" "$VERSION" "$DEST/$CAM" || { rm -rf "${DEST:?}/$CAM"; exit 1; }
chmod -R a-w "$DEST/$CAM"
