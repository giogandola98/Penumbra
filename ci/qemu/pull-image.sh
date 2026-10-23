#!/bin/bash
# Pull the QEMU-EOS-Optimized image pinned in ci/qemu-image.txt (CI host side). Prints the image reference.
# The image is built, tested and published by the QEMU-EOS-Optimized repo's CI into the container registry of a
# private organisation; it is pinned by digest, so a pull can never change what we test with.
# The file holds only <name>@sha256:<digest>; the registry host is the CI server ($GITHUB_SERVER_URL) and the owner
# is the CI variable $ROM_PKG_OWNER (same private org as the ROM packages).
# Auth: $ROM_TOKEN (read:package, member of that org). Credentials go to a throw-away authfile:
# the runner is a shared root host, never ~/.config or /run.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
name=$(grep -v '^#' "$ROOT/ci/qemu-image.txt" | grep -m1 .)
case "$name" in *@sha256:*) ;; *) echo "ci/qemu-image.txt must pin a digest (name@sha256:...)" >&2; exit 1;; esac
: "${GITHUB_SERVER_URL:?}" "${ROM_PKG_OWNER:?CI variable ROM_PKG_OWNER (private org) is required}"
host=${GITHUB_SERVER_URL#*://}; host=${host%%/*}
ref="$host/$ROM_PKG_OWNER/$name"

if podman image exists "$ref"; then
    echo "image $ref already present" >&2
else
    [ -n "${ROM_TOKEN:-}" ] || { echo "ROM_TOKEN not set: cannot pull the private QEMU image" >&2; exit 1; }
    tmp=$(mktemp -d); chmod 700 "$tmp"
    trap 'rm -rf "$tmp"' EXIT
    # Plain-HTTP registry on the LAN, hence --tls-verify=false; the digest pin guarantees the content.
    printf '%s' "$ROM_TOKEN" | podman login --tls-verify=false --authfile "$tmp/auth.json" \
        -u "${REGISTRY_USER:-root}" --password-stdin "${ref%%/*}" >/dev/null
    unset ROM_TOKEN
    start=$(date +%s)
    podman pull -q --tls-verify=false --authfile "$tmp/auth.json" "$ref" >&2
    echo "pulled $ref in $(( $(date +%s) - start ))s" >&2
fi
echo "$ref"
