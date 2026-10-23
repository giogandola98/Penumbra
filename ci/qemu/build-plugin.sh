#!/bin/bash
# Compile ci/qemu/plugins/wakeups.c against the plugin header shipped in the ml-qemu image.
# Compiled at job time (not in the image) so a plugin change never triggers a QEMU rebuild.
# Usage: ci/qemu/build-plugin.sh OUT_DIR   -> OUT_DIR/libwakeups.so
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
QEMU_EOS=${QEMU_EOS:-/opt/qemu-eos}
out=${1:?usage: build-plugin.sh OUT_DIR}
mkdir -p "$out"
gcc -shared -fPIC -O2 -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Werror \
    -I"$QEMU_EOS/include" -o "$out/libwakeups.so" "$HERE/plugins/wakeups.c"
echo "$out/libwakeups.so"
