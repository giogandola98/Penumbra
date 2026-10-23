#!/bin/bash
# Boot a Canon camera in qemu-eos headless and compare idle wakeups across scenarios:
#   canon     Canon firmware only (camera bootflag off, card without ML)
#   ml-full   upstream-equivalent ML build (ML_PROFILE=full)        if ML_ZIP_FULL is set
#   ml-photo  this fork's photo build (ML_PROFILE=photo, EOSM only)  if ML_ZIP_PHOTO is set
# Runs inside the qemu-eos image pinned in ci/qemu-image.txt. Without ROMs it prints SKIPPED and exits 0.
#
# Environment:
#   CAM            camera model as qemu-eos names it (default EOSM; e.g. 1100D). ML platform dir is
#                  platform/CAM.<fw> (EOSM -> EOSM.202, 1100D -> 1100D.105).
#   ML_ROM_DIR     dir containing CAM/ROM0.BIN ROM1.BIN [SFDATA.BIN] (default /roms); never written.
#   ML_ZIP_FULL, ML_ZIP_PHOTO   built magiclantern.zip of each profile (optional)
#   WARMUP_S, MEASURE_S         boot time excluded from, and length of, the measurement window (40, 30)
#   TASKS=1        also count DryOS task switches with qemu-eos `-d tasks` (default 1)
#   OUT            output dir (default emu-out/CAM)
#   PARALLEL=1     run the scenarios at once, if the host has the cores (default 0: one after the other)
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

# The wakeups plugin ships in the QEMU-EOS-Optimized image (ci/qemu-image.txt), built with its QEMU.
PLUGIN=${PLUGIN:-/opt/qemu-eos/plugins/libwakeups.so}
[ -f "$PLUGIN" ] || { echo "no wakeups plugin at $PLUGIN (wrong image?)" >&2; exit 2; }
debugmsg=$(sed -n 's/^NSTUB( *\(0x[0-9A-Fa-f]*\), *DryosDebugMsg).*/\1/p' "$PLAT/stubs.S" | head -1)
echo "CAM=$CAM platform=${plats[0]} DryosDebugMsg=${debugmsg:-unknown} qemu-eos=$(cat /opt/qemu-eos/QEMU_EOS_SHA 2>/dev/null || echo ?)"

scenarios=(canon)
[ -n "${ML_ZIP_FULL:-}" ] && scenarios+=(ml-full)
[ -n "${ML_ZIP_PHOTO:-}" ] && scenarios+=(ml-photo)

run_scenario() {   # $1 = scenario; disk images and console output are per scenario, so runs can overlap
    local sc=$1 zip boot seeds=()
    case $sc in
        canon)    zip=;               boot=   ;;
        ml-full)  zip=$ML_ZIP_FULL;   boot=--boot ;;
        ml-photo) zip=$ML_ZIP_PHOTO;  boot=--boot ;;
    esac
    if [ -n "$zip" ]; then      # see make_sd_image.py --seed: skip ML's first-boot ROM backup
        for r in ROM0 ROM1; do
            [ -f "$romdir/$r.BIN" ] && seeds+=(--seed "ML/LOGS/$r.BIN:$(stat -c %s "$romdir/$r.BIN")")
        done
    fi
    python3 -I "$HERE/make_sd_image.py" "${seeds[@]}" "$ROOT/platform/sd.qcow2.xz" "$work/$sc-sd.img" ${zip:+"$zip"}
    python3 -I "$HERE/make_sd_image.py" "$ROOT/platform/sd.qcow2.xz" "$work/$sc-cf.img" > /dev/null
    local st=0
    python3 -I "$HERE/scenario.py" --cam "$CAM" --rom-dir "$ML_ROM_DIR" \
        --sd "$work/$sc-sd.img" --cf "$work/$sc-cf.img" $boot --plugin "$PLUGIN" \
        ${debugmsg:+--debugmsg-addr "$debugmsg"} $([ "$TASKS" = 1 ] && echo --tasks) \
        --warmup "$WARMUP_S" --measure "$MEASURE_S" --out "$OUT/$sc" || st=$?
    rm -f "$work/$sc-sd.img" "$work/$sc-cf.img"
    return $st
}

# PARALLEL=1: all scenarios at once. Each is a fixed wall-clock timeline (warmup + window) that keeps one
# host core busy at most, so the camera takes one timeline instead of one per scenario (QEMU-EOS-Optimized
# bench: same screenshots and rates, pair wall time -48 %). Not the default yet: in CI runs 34/38 the
# plugin's window started ~11.7 s late (result.json plugin_clock_lag_s), i.e. QEMU loaded its plugins
# that long after scenario.py started it, so the measured window was 18-22 s instead of 30 s.
rc=0
if [ "${PARALLEL:-0}" = 1 ] && [ "$(nproc)" -ge "${#scenarios[@]}" ]; then
    pids=()
    for sc in "${scenarios[@]}"; do
        mkdir -p "$OUT/$sc"
        run_scenario "$sc" > "$OUT/$sc/console.txt" 2>&1 &
        pids+=($!)
    done
    for i in "${!scenarios[@]}"; do
        st=0; wait "${pids[$i]}" || st=$?
        echo "== $CAM / ${scenarios[$i]} (parallel)"; cat "$OUT/${scenarios[$i]}/console.txt"
        [ "$st" -eq 0 ] || { echo "scenario ${scenarios[$i]} FAILED"; rc=1; }
    done
else
    for sc in "${scenarios[@]}"; do
        echo "== $CAM / $sc"
        run_scenario "$sc" || { echo "scenario $sc FAILED"; rc=1; }
    done
fi

python3 -I "$HERE/compare.py" "$CAM" "$OUT" "${scenarios[@]}" | tee "$OUT/comparison.md"
exit $rc
