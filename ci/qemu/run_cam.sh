#!/bin/bash
# Boot a Canon camera in qemu-eos headless and compare idle wakeups across scenarios:
#   canon     Canon firmware only (camera bootflag off, card without ML)
#   ml-full   upstream-equivalent ML build (ML_PROFILE=full)        if ML_ZIP_FULL is set
#   ml-photo  this fork's photo build (ML_PROFILE=photo, EOSM only)  if ML_ZIP_PHOTO is set
# Runs inside the ml-qemu image. Without ROMs it prints SKIPPED and exits 0.
#
# Environment:
#   CAM            camera model as qemu-eos names it (default EOSM; e.g. 1100D). ML platform dir is
#                  platform/CAM.<fw> (EOSM -> EOSM.202, 1100D -> 1100D.105).
#   ML_ROM_DIR     dir containing CAM/ROM0.BIN ROM1.BIN [SFDATA.BIN] (default /roms); never written.
#   ML_ZIP_FULL, ML_ZIP_PHOTO   built magiclantern.zip of each profile (optional)
#   WARMUP_S, MEASURE_S         boot time excluded from, and length of, the measurement window (40, 30)
#   TASKS=1        also count DryOS task switches with qemu-eos `-d tasks` (default 1)
#   OUT            output dir (default emu-out/CAM)
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
CAM=${CAM:-EOSM}
ML_ROM_DIR=${ML_ROM_DIR:-/roms}
WARMUP_S=${WARMUP_S:-40}
MEASURE_S=${MEASURE_S:-30}
TASKS=${TASKS:-1}
OUT=${OUT:-$ROOT/emu-out/$CAM}

case "$CAM" in *[!A-Za-z0-9]*|"") echo "bad CAM '$CAM'" >&2; exit 2;; esac
mapfile -t plats < <(cd "$ROOT/platform" && ls -d "$CAM".[0-9]* 2>/dev/null)
[ "${#plats[@]}" -eq 1 ] || { echo "need exactly one platform/$CAM.<fw>, found: ${plats[*]:-none}" >&2; exit 2; }
PLAT=$ROOT/platform/${plats[0]}

skip() {
    mkdir -p "$OUT"
    echo "SKIPPED: $1" | tee "$OUT/comparison.md"
    exit 0
}

romdir=$ML_ROM_DIR/$CAM
[ -f "$romdir/ROM1.BIN" ] || skip "no ROM for $CAM (expected $romdir/ROM0.BIN, ROM1.BIN [, SFDATA.BIN])"
[ -f "$romdir/ROM0.BIN" ] || echo "note: no ROM0.BIN for $CAM (some models only need ROM1)"
[ -f "$romdir/SFDATA.BIN" ] || echo "note: no SFDATA.BIN for $CAM (serial flash will be blank)"

"$HERE/check_rom_md5.sh" "${plats[0]/./-}" "$romdir"

rm -rf "$OUT" && mkdir -p "$OUT"
work=$(mktemp -d)                      # disk images: rebuilt per scenario, never uploaded
trap 'rm -rf "$work"' EXIT

PLUGIN=$("$HERE/build-plugin.sh" "$work")
debugmsg=$(sed -n 's/^NSTUB( *\(0x[0-9A-Fa-f]*\), *DryosDebugMsg).*/\1/p' "$PLAT/stubs.S" | head -1)
echo "CAM=$CAM platform=${plats[0]} DryosDebugMsg=${debugmsg:-unknown} qemu-eos=$(cat /opt/qemu-eos/QEMU_EOS_SHA 2>/dev/null || echo ?)"

scenarios=(canon)
[ -n "${ML_ZIP_FULL:-}" ] && scenarios+=(ml-full)
[ -n "${ML_ZIP_PHOTO:-}" ] && scenarios+=(ml-photo)

rc=0
for sc in "${scenarios[@]}"; do
    echo "== $CAM / $sc"
    case $sc in
        canon)    zip=;               boot=   ;;
        ml-full)  zip=$ML_ZIP_FULL;   boot=--boot ;;
        ml-photo) zip=$ML_ZIP_PHOTO;  boot=--boot ;;
    esac
    python3 -I "$HERE/make_sd_image.py" "$ROOT/platform/sd.qcow2.xz" "$work/sd.img" ${zip:+"$zip"}
    python3 -I "$HERE/make_sd_image.py" "$ROOT/platform/sd.qcow2.xz" "$work/cf.img" > /dev/null
    python3 -I "$HERE/scenario.py" --cam "$CAM" --rom-dir "$ML_ROM_DIR" \
        --sd "$work/sd.img" --cf "$work/cf.img" $boot --plugin "$PLUGIN" \
        ${debugmsg:+--debugmsg-addr "$debugmsg"} $([ "$TASKS" = 1 ] && echo --tasks) \
        --warmup "$WARMUP_S" --measure "$MEASURE_S" --out "$OUT/$sc" || { echo "scenario $sc FAILED"; rc=1; }
    rm -f "$work/sd.img" "$work/cf.img"
done

python3 -I "$HERE/compare.py" "$CAM" "$OUT" "${scenarios[@]}" | tee "$OUT/comparison.md"
exit $rc
