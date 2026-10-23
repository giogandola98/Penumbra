#!/bin/bash
# Upload one camera's ROM dumps to the private Forgejo generic package used by CI (ci/qemu/fetch_roms.sh).
# Local helper for the maintainer; never run in CI. See docs/EMULATION.md "ROM handling".
#
# Usage: tools/upload_roms.sh [--replace] [--allow-public-owner] CAM DIR
#   CAM   camera model as in platform/CAM.<fw> (EOSM, 1100D, ...)
#   DIR   directory containing ROM0.BIN, ROM1.BIN and optionally SFDATA.BIN
#         (e.g. ~/Documents/roms/1100D)
# Package: owner $ROM_PKG_OWNER (required; a private org), name canon-rom, version CAM-<fw> (EOSM-202, 1100D-105).
#   --replace             delete an existing package version first
#   --allow-public-owner  upload even if the owner is publicly visible (packages would be public too)
# Credentials: FORGEJO_URL, FORGEJO_USER, FORGEJO_PASSWORD from the environment, or from the shell file
# named by $FORGEJO_CREDENTIALS (kept outside the repo); they reach curl through stdin (-K -), never argv.
set -euo pipefail
replace=0 allow_public=0
while [ $# -gt 0 ]; do
    case $1 in
        --replace) replace=1; shift ;;
        --allow-public-owner) allow_public=1; shift ;;
        -*) echo "unknown option $1" >&2; exit 2 ;;
        *) break ;;
    esac
done
CAM=${1:?usage: upload_roms.sh [--replace] CAM DIR}
DIR=${2:?usage: upload_roms.sh [--replace] CAM DIR}
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
OWNER=${ROM_PKG_OWNER:?set ROM_PKG_OWNER to the private owner of the ROM package}
PKG=canon-rom

case "$CAM" in *[!A-Za-z0-9]*|"") echo "bad CAM '$CAM'" >&2; exit 2;; esac
plat=$(cd "$ROOT/platform" && ls -d "$CAM".[0-9]* 2>/dev/null | head -1)
[ -n "$plat" ] || { echo "no platform/$CAM.<fw> in this tree" >&2; exit 2; }
VERSION=$CAM-${plat#*.}

files=()
for f in ROM0.BIN ROM1.BIN SFDATA.BIN; do
    [ -f "$DIR/$f" ] && files+=("$f")
done
[ -f "$DIR/ROM1.BIN" ] || { echo "$DIR/ROM1.BIN missing" >&2; exit 2; }

if [ -n "${FORGEJO_CREDENTIALS:-}" ]; then
    # shellcheck disable=SC1090
    . "$FORGEJO_CREDENTIALS"
fi
: "${FORGEJO_URL:?}" "${FORGEJO_USER:?}" "${FORGEJO_PASSWORD:?}"

fj() {   # fj CURL_ARGS... : authenticated curl, credentials via stdin config
    printf 'user = "%s:%s"\n' "$FORGEJO_USER" "$FORGEJO_PASSWORD" | curl -s -K - "$@"
}

# The package inherits the owner's visibility: a public owner means anonymously downloadable ROMs.
vis=$(fj "$FORGEJO_URL/api/v1/orgs/$OWNER" | python3 -I -c 'import json,sys; print(json.load(sys.stdin).get("visibility",""))' 2>/dev/null || true)
[ -n "$vis" ] || vis=$(fj "$FORGEJO_URL/api/v1/users/$OWNER" | python3 -I -c 'import json,sys; print(json.load(sys.stdin).get("visibility",""))' 2>/dev/null || true)
if [ -z "$vis" ]; then
    echo "owner '$OWNER' not found. Create a private org first, e.g.:" >&2
    echo "  POST $FORGEJO_URL/api/v1/orgs {\"username\":\"$OWNER\",\"visibility\":\"private\"}" >&2
    exit 1
fi
if [ "$vis" = public ] && [ "$allow_public" = 0 ]; then
    echo "owner '$OWNER' is public: its packages would be downloadable by anyone. Refusing." >&2
    exit 1
fi
echo "owner $OWNER ($vis), package $PKG, version $VERSION, files: ${files[*]}"

base="$FORGEJO_URL/api/packages/$OWNER/generic/$PKG/$VERSION"
if [ "$replace" = 1 ]; then
    code=$(fj -o /dev/null -w '%{http_code}' -X DELETE "$FORGEJO_URL/api/v1/packages/$OWNER/generic/$PKG/$VERSION")
    echo "delete existing version: HTTP $code"
fi

for f in "${files[@]}"; do
    code=$(fj -o /dev/null -w '%{http_code}' --upload-file "$DIR/$f" "$base/$f")
    case $code in
        201) echo "uploaded $f" ;;
        409) echo "$f already exists in $VERSION (use --replace)" >&2; exit 1 ;;
        *)   echo "upload of $f failed: HTTP $code" >&2; exit 1 ;;
    esac
done

# Verify what the server stored against the local files.
listing=$(fj "$FORGEJO_URL/api/v1/packages/$OWNER/generic/$PKG/$VERSION/files")
for f in "${files[@]}"; do
    want=$(sha256sum "$DIR/$f" | cut -d' ' -f1)
    got=$(printf '%s' "$listing" | python3 -I -c 'import json,sys; print(next((x["sha256"] for x in json.load(sys.stdin) if x["name"]==sys.argv[1]), ""))' "$f")
    [ "$want" = "$got" ] || { echo "server sha256 mismatch for $f" >&2; exit 1; }
    echo "verified $f sha256=$want"
done

echo
echo "MD5 lines for docs/firmware/rom-md5.txt (checksums only, safe to commit):"
for f in "${files[@]}"; do
    echo "$VERSION $f $(md5sum "$DIR/$f" | cut -d' ' -f1)"
done
