#!/bin/bash
# Pull the QEMU-EOS-Optimized image pinned in ci/qemu-image.txt (CI host side). Prints the image reference.
# The image is built, tested and published by the QEMU-EOS-Optimized repo's CI into the private
# <private-org> container registry; it is pinned by digest, so a pull can never change what we test with.
# Auth: $ROM_TOKEN (read:package, member of <private-org>). Credentials go to a throw-away authfile:
# the runner is a shared root host, never ~/.config or /run.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
ref=$(grep -v '^#' "$ROOT/ci/qemu-image.txt" | grep -m1 .)
case "$ref" in *@sha256:*) ;; *) echo "ci/qemu-image.txt must pin a digest (repo@sha256:...)" >&2; exit 1;; esac

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
