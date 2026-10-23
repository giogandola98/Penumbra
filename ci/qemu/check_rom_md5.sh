#!/bin/bash
# Verify ROM dumps against docs/firmware/rom-md5.txt (lines "<CAM>-<FW> <FILE> <md5>", '#' comments).
# The dumps differ per camera unit, so a mismatch means a wrong or corrupted file: fail loudly.
# A version with no lines in the file is accepted with a note (checksums not recorded yet).
# Usage: check_rom_md5.sh CAM-FW DIR      e.g. check_rom_md5.sh 1100D-105 /roms/1100D
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
ver=${1:?usage: check_rom_md5.sh CAM-FW DIR}
dir=${2:?usage: check_rom_md5.sh CAM-FW DIR}
md5file=$ROOT/docs/firmware/rom-md5.txt

n=0
if [ -f "$md5file" ]; then
    while read -r v f m _; do
        [ "$v" = "$ver" ] || continue
        [ -f "$dir/$f" ] || { echo "ROM check: $f listed for $ver but missing" >&2; exit 1; }
        actual=$(md5sum "$dir/$f" | cut -c1-32)
        [ "$actual" = "$m" ] || { echo "ROM check: MD5 mismatch for $ver $f" >&2; exit 1; }
        n=$((n + 1))
    done < "$md5file"
fi
if [ "$n" -eq 0 ]; then
    echo "ROM check: no MD5 recorded for $ver in docs/firmware/rom-md5.txt (not verified)"
else
    echo "ROM check: $n file(s) of $ver match docs/firmware/rom-md5.txt"
fi
