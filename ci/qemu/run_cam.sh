#!/bin/bash
# Boot a Canon camera in qemu-eos headless and compare idle wakeups across scenarios:
#   canon     Canon firmware only (camera bootflag off, card without ML)
#   ml-full   upstream-equivalent ML build (ML_PROFILE=full)        if ML_ZIP_FULL is set
#   ml-photo  this fork's photo build (ML_PROFILE=photo, EOSM only)  if ML_ZIP_PHOTO is set
#   ml-hold   ml-full built with POWERSAVE_HOLD=y (1100D)             if ML_ZIP_HOLD is set
#   ml-hold-ivl  ml-hold with the intervalometer auto-started by a config preset (an active hold)
#                                                                    if ML_ZIP_HOLD and DISPLAY_OFF_S > 0
# Runs inside the qemu-eos image pinned in ci/qemu-image.txt. Without ROMs it prints SKIPPED and exits 0.
#
# Environment:
#   CAM            camera model as qemu-eos names it (default EOSM; e.g. 1100D). ML platform dir is
#                  platform/CAM.<fw> (EOSM -> EOSM.202, 1100D -> 1100D.105).
#   ML_ROM_DIR     dir containing CAM/ROM0.BIN ROM1.BIN [SFDATA.BIN] (default /roms); never written.
#   ML_ZIP_FULL, ML_ZIP_PHOTO, ML_ZIP_HOLD   built magiclantern.zip of each variant (optional)
#   WARMUP_S, MEASURE_S         boot time excluded from, and length of, the measurement window (40, 30)
#   TASKS=1        also count DryOS task switches with qemu-eos `-d tasks` (default 1)
#   DISPLAY_OFF_S  after the window: display off (INFO/DISP) for this long, then wake it with the same key
#                  (deep-sleep check, docs/analysis/powersave-hold.md; default 30 on the 1100D, else 0)
#   OUT            output dir (default emu-out/CAM)
#   PARALLEL=0     run the scenarios one after the other (default 1: at once, if the host has the cores)
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

# Canon's power-save counter in RAM (docs/firmware/1100D-powersave.md), sampled during the window.
# Deep sleep (selfRefresh) is modelled by qemu-eos for the 1100D only.
case "$CAM" in 1100D) pmctr=0x2B64; DISPLAY_OFF_S=${DISPLAY_OFF_S:-30} ;; *) pmctr=; DISPLAY_OFF_S=${DISPLAY_OFF_S:-0} ;; esac

scenarios=(canon)
[ -n "${ML_ZIP_FULL:-}" ] && scenarios+=(ml-full)
[ -n "${ML_ZIP_PHOTO:-}" ] && scenarios+=(ml-photo)
[ -n "${ML_ZIP_HOLD:-}" ] && scenarios+=(ml-hold)
[ -n "${ML_ZIP_HOLD:-}" ] && [ "${DISPLAY_OFF_S%.*}" -gt 0 ] && scenarios+=(ml-hold-ivl)

# ml-hold-ivl: ML config preset that auto-starts the intervalometer at boot (trigger "Leave menu" = 0),
# first shot after 15 min, so it holds power save for the whole run without taking a picture.
printf '%s\r\n' "interval.enabled = 1" "interval.time = 600" "interval.start.time = 900" > "$work/ivl.cfg"

run_scenario() {   # $1 = scenario; disk images and console output are per scenario, so runs can overlap
    local sc=$1 zip boot seeds=() extra=()
    case $sc in
        canon)    zip=;               boot=   ;;
        ml-full)  zip=$ML_ZIP_FULL;   boot=--boot ;;
        ml-photo) zip=$ML_ZIP_PHOTO;  boot=--boot ;;
        ml-hold)  zip=$ML_ZIP_HOLD;   boot=--boot ;;
        ml-hold-ivl) zip=$ML_ZIP_HOLD; boot=--boot; seeds+=(--put "ML/SETTINGS/MAGIC.CFG:$work/ivl.cfg"); extra+=(--no-menu --active-hold intervalometer) ;;
    esac
    [ -n "$boot" ] && extra+=(--config-check)
    if [ -n "$zip" ]; then      # see make_sd_image.py --seed: skip ML's first-boot ROM backup
        for r in ROM0 ROM1; do
            [ -f "$romdir/$r.BIN" ] && seeds+=(--seed "ML/LOGS/$r.BIN:$(stat -c %s "$romdir/$r.BIN")")
        done
    fi
    python3 -I "$HERE/make_sd_image.py" "${seeds[@]}" "$ROOT/platform/sd.qcow2.xz" "$work/$sc-sd.img" ${zip:+"$zip"} \
        | tee "$work/$sc-sd.txt"
    python3 -I "$HERE/make_sd_image.py" "$ROOT/platform/sd.qcow2.xz" "$work/$sc-cf.img" > /dev/null
    local st=0
    python3 -I "$HERE/scenario.py" --cam "$CAM" --rom-dir "$ML_ROM_DIR" \
        --sd "$work/$sc-sd.img" --cf "$work/$sc-cf.img" $boot --plugin "$PLUGIN" \
        ${debugmsg:+--debugmsg-addr "$debugmsg"} $([ "$TASKS" = 1 ] && echo --tasks) \
        ${pmctr:+--pm-counter-addr "$pmctr"} --display-off-s "$DISPLAY_OFF_S" "${extra[@]}" \
        --warmup "$WARMUP_S" --measure "$MEASURE_S" --out "$OUT/$sc" || st=$?
    # ML's settings on the card after the run: did the config save after the deep sleep work?
    local off
    off=$(sed -n 's/.*FAT at offset \([0-9]*\).*/\1/p' "$work/$sc-sd.txt")
    if [ -n "$zip" ] && [ -n "$off" ]; then
        { MTOOLS_SKIP_CHECK=1 mdir -i "$work/$sc-sd.img@@$off" -b ::/ML/SETTINGS 2>&1
          echo "--- MAGIC.CFG"
          MTOOLS_SKIP_CHECK=1 mtype -i "$work/$sc-sd.img@@$off" ::/ML/SETTINGS/MAGIC.CFG 2>&1 | head -c 4096
        } > "$OUT/$sc/sd_settings.txt" || true
    fi
    rm -f "$work/$sc-sd.img" "$work/$sc-cf.img"
    return $st
}

# PARALLEL=1 (default): all scenarios at once. Each is a fixed wall-clock timeline (warmup + window) that
# keeps one host core busy at most, so the camera takes one timeline instead of one per scenario
# (QEMU-EOS-Optimized bench: same screenshots and rates, pair wall time -48 %). PARALLEL=0: one by one.
rc=0
if [ "${PARALLEL:-1}" = 1 ] && [ "$(nproc)" -ge "${#scenarios[@]}" ]; then
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

# With the display-off segment, compare.py also checks the deep-sleep expectations (DEEP_SLEEP_ASSERT=0: report only)
[ "${DISPLAY_OFF_S%.*}" -gt 0 ] && export DEEP_SLEEP_ASSERT=${DEEP_SLEEP_ASSERT:-1}
python3 -I "$HERE/compare.py" "$CAM" "$OUT" "${scenarios[@]}" | tee "$OUT/comparison.md" || rc=1
exit $rc
