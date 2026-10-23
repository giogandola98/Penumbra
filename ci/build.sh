#!/bin/bash
# Build EOSM.202 and write build-report.txt (size metrics + deltas vs ci/baseline.txt).
# Used by Forgejo CI and locally. Run from anywhere; output lands in the repo root.
# Usage: ci/build.sh [--update-baseline]
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
PLAT=$ROOT/platform/EOSM.202
REPORT=$ROOT/build-report.txt
BASELINE=$ROOT/ci/baseline.txt

# Local hosts use the pinned env script; the CI image already has the toolchain on PATH.
if ! command -v arm-none-eabi-gcc >/dev/null 2>&1 && [ -f "$HOME/.local/opt/ml-env.sh" ]; then
    # shellcheck disable=SC1091
    . "$HOME/.local/opt/ml-env.sh"
fi

make -C "$PLAT" -j"$(nproc)" FATAL_WARNINGS=y

AUTOEXEC=$PLAT/autoexec.bin
[ -f "$AUTOEXEC" ] || AUTOEXEC=$PLAT/build/autoexec.bin
ZIP=$PLAT/build/magiclantern.zip
ELF=$PLAT/build/magiclantern

autoexec_bytes=$(stat -c %s "$AUTOEXEC")
read -r text data bss _ < <(arm-none-eabi-size "$ELF" | awk 'NR==2')
# python's zipfile: no dependency on an unzip binary
modules=$(python3 -I -c 'import sys,zipfile; print(sum(n.endswith(".mo") for n in zipfile.ZipFile(sys.argv[1]).namelist()))' "$ZIP")
tasks=$(grep -rEho 'TASK_CREATE|task_create' "$ROOT/src" --include='*.c' --include='*.h' | wc -l)

declare -A cur=([autoexec_bytes]=$autoexec_bytes [text]=$text [data]=$data [bss]=$bss [modules]=$modules [task_create_sites]=$tasks)
keys=(autoexec_bytes text data bss modules task_create_sites)

if [ "${1:-}" = "--update-baseline" ]; then
    for k in "${keys[@]}"; do echo "$k=${cur[$k]}"; done > "$BASELINE"
    echo "baseline updated: $BASELINE"
fi

declare -A base=()
if [ -f "$BASELINE" ]; then
    while IFS='=' read -r k v; do [[ $k =~ ^[a-z_]+$ ]] && base[$k]=$v; done < "$BASELINE"
fi

{
    echo "ML-Optimized build report (EOSM.202)"
    echo "commit: $(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
    echo
    echo "arm-none-eabi-size build/magiclantern:"
    arm-none-eabi-size "$ELF"
    echo
    printf '%-20s %10s %10s %8s\n' metric current baseline delta
    for k in "${keys[@]}"; do
        b=${base[$k]:-}
        if [ -n "$b" ]; then d=$(printf '%+d' $((cur[$k] - b))); else b=n/a; d=n/a; fi
        printf '%-20s %10s %10s %8s\n' "$k" "${cur[$k]}" "$b" "$d"
    done
} | tee "$REPORT"
