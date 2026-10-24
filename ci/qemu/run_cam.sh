#!/bin/bash
# Boot a Canon camera in qemu-eos headless and compare idle wakeups across scenarios:
#   canon     Canon firmware only (camera bootflag off, card without ML)
#   ml-full   upstream-equivalent ML build (ML_PROFILE=full)        if ML_ZIP_FULL is set
#   ml-photo  this fork's photo build (ML_PROFILE=photo: EOSM, 4000D) if ML_ZIP_PHOTO is set
#   ml-hold   ml-full built with POWERSAVE_HOLD=y (1100D)             if ML_ZIP_HOLD is set
#   ml-hold-ivl  ml-hold with the intervalometer auto-started by a config preset (an active hold)
#                                                                    if ML_ZIP_HOLD and DISPLAY_OFF_S > 0
#   ml-photo-ivl ml-photo with the same preset                      if ML_ZIP_PHOTO and IVL_PHOTO=1 (4000D)
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
#                  (deep-sleep check, docs/analysis/powersave-hold.md; default 30 on the 1100D and 4000D, else 0)
#   Per-camera values (PM_COUNTER_ADDR, DISPLAY_OFF_S, CANON_READY, SHOOT_*, PROBE_KEYS, MENU_TOUR, IVL_PHOTO,
#                  PHOTO_MODULES, EXPECT_SLEEP) come from platform/CAM.<fw>/emulation.sh; the environment overrides.
#   OUT            output dir (default emu-out/CAM)
#   SHOOT=0        skip the shooting scenarios (shoot.py: Canon shot, ML intervalometer, HDR, bulb)
#   PARALLEL       1: scenarios at once; 0: one after the other; auto (default): at once only if the host
#                  is not loaded (1-min load average + scenarios <= 1.5 x nproc)
# A scenario that ends in an INFRASTRUCTURE TIMEOUT (scenario.py exit 3: the emulator never reached a
# step of its timeline) is run once more, alone; a second timeout fails the job as infrastructure.
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

# Per-camera values (power-save counter, display-off segment, ready markers, shooting scenarios, port
# bring-up options, ...) live in platform/CAM.<fw>/emulation.sh; see docs/EMULATION.md, "Adding a camera".
# A platform without one runs with the generic defaults below. Environment variables override either.
# shellcheck source=/dev/null
[ -f "$PLAT/emulation.sh" ] && . "$PLAT/emulation.sh"
PM_COUNTER_ADDR=${PM_COUNTER_ADDR-}      # RAM address of Canon's power-save counter (none: not sampled)
DISPLAY_OFF_S=${DISPLAY_OFF_S:-0}        # display-off (deep-sleep check) segment length, 0 = none
CANON_READY=${CANON_READY-}              # guest ready marker of the canon scenario, "" = timed from launch
# Guest ready (scenario.py --ready-marker, DryosDebugMsg log): the window is timed from it, not from launch.
# Canon: the display is on at the end of boot (1100D; other models: a stable screen). ML: its boot hold
# is released (src/powersave.c), i.e. ML's init, ROM backup and module loading are over.
canon_ready=(); [ -n "$CANON_READY" ] && canon_ready=(--ready-marker "$CANON_READY")
ml_ready=(--ready-marker '\[PS\] release boot ->')
PROBE_KEYS=${PROBE_KEYS-}                # port bring-up: keys probed in the canon scenario (--probe-keys)
MENU_TOUR=${MENU_TOUR-}                  # port bring-up: ML menu tabs walked in the ML scenarios (--menu-tour)
IVL_PHOTO=${IVL_PHOTO:-0}                # 1: ml-photo-ivl (intervalometer preset, photo build)
PHOTO_MODULES=${PHOTO_MODULES-}          # modules switched on (ML/SETTINGS/<name>.en) in the photo scenarios
# Shooting scenarios (shoot.py reads these from the environment) and the deep-sleep expectations that differ
# from the generic ones in compare.py.
export SHOOT_CANON_READY=${SHOOT_CANON_READY-} SHOOT_SHUTTER_KEY=${SHOOT_SHUTTER_KEY-}
export SHOOT_SCENARIOS=${SHOOT_SCENARIOS-} SHOOT_NO_FILES=${SHOOT_NO_FILES-} EXPECT_SLEEP=${EXPECT_SLEEP-}

# Stubs against their ROM evidence (tools/check_stubs.py; skipped for platforms without an evidence file).
python3 -I "$ROOT/tools/check_stubs.py" "$PLAT" "$romdir/ROM1.BIN" | tee "$OUT/stubs-check.txt" || { echo "stub check FAILED"; exit 1; }

scenarios=(canon)
[ -n "${ML_ZIP_FULL:-}" ] && scenarios+=(ml-full)
[ -n "${ML_ZIP_PHOTO:-}" ] && scenarios+=(ml-photo)
[ -n "${ML_ZIP_HOLD:-}" ] && scenarios+=(ml-hold)
[ -n "${ML_ZIP_HOLD:-}" ] && [ "${DISPLAY_OFF_S%.*}" -gt 0 ] && scenarios+=(ml-hold-ivl)
[ -n "${ML_ZIP_PHOTO:-}" ] && [ "$IVL_PHOTO" = 1 ] && scenarios+=(ml-photo-ivl)

# ml-hold-ivl: ML config preset that auto-starts the intervalometer at boot (trigger "Leave menu" = 0),
# first shot after 15 min, so it holds power save for the whole run without taking a picture.
printf '%s\r\n' "interval.enabled = 1" "interval.time = 600" "interval.start.time = 900" > "$work/ivl.cfg"

run_scenario() {   # $1 = scenario; disk images and console output are per scenario, so runs can overlap
    local sc=$1 zip boot seeds=() extra=()
    case $sc in
        canon)    zip=;               boot=; [ -n "$PROBE_KEYS" ] && extra+=(--probe-keys "$PROBE_KEYS") ;;
        ml-full)  zip=$ML_ZIP_FULL;   boot=--boot ;;
        ml-photo) zip=$ML_ZIP_PHOTO;  boot=--boot ;;
        ml-hold)  zip=$ML_ZIP_HOLD;   boot=--boot ;;
        ml-hold-ivl|ml-photo-ivl)
                  zip=${ML_ZIP_HOLD:-}; [ "$sc" = ml-photo-ivl ] && zip=$ML_ZIP_PHOTO
                  boot=--boot; seeds+=(--put "ML/SETTINGS/MAGIC.CFG:$work/ivl.cfg"); extra+=(--no-menu --active-hold intervalometer) ;;
    esac
    case $sc in ml-full|ml-photo|ml-hold) [ -n "$MENU_TOUR" ] && extra+=(--menu-tour "$MENU_TOUR") ;; esac
    case $sc in ml-photo|ml-photo-ivl)
        : > "$work/module.en"
        for m in $PHOTO_MODULES; do seeds+=(--put "ML/SETTINGS/$(echo "$m" | tr a-z A-Z).EN:$work/module.en"); done
        [ -n "$PHOTO_MODULES" ] && extra+=(--expect-modules "$(echo $PHOTO_MODULES | tr ' ' ,)") ;;
    esac
    [ -n "$boot" ] && extra+=(--config-check)
    extra+=(${canon_ready[@]+"${canon_ready[@]}"})
    [ -n "$boot" ] && extra+=("${ml_ready[@]}")
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
        ${PM_COUNTER_ADDR:+--pm-counter-addr "$PM_COUNTER_ADDR"} --display-off-s "$DISPLAY_OFF_S" "${extra[@]}" \
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

# Parallel: all scenarios at once. Each timeline keeps one host core busy at most, so the camera takes
# one timeline instead of one per scenario (QEMU-EOS-Optimized bench: same screenshots and rates, pair
# wall time -48 %). On a loaded host the scenarios would starve each other (and the monitor): one by one.
rc=0
retry=()
report() {   # $1 scenario, $2 exit status, $3 label
    case $2 in
        0) ;;
        3) echo "scenario $1: INFRASTRUCTURE TIMEOUT$3 (emulator too slow or unresponsive; not a functional result)"
           retry+=("$1") ;;
        *) echo "scenario $1 FAILED$3"; rc=1 ;;
    esac
}
par=${PARALLEL:-auto}
if [ "$par" = auto ]; then
    load=$(cut -d' ' -f1 /proc/loadavg)
    par=$(awk -v n="$(nproc)" -v l="$load" -v k="${#scenarios[@]}" 'BEGIN { print (l + k <= 1.5 * n) ? 1 : 0 }')
    echo "PARALLEL=auto: $(nproc) CPUs, load $load, ${#scenarios[@]} scenarios -> PARALLEL=$par"
fi
if [ "$par" = 1 ] && [ "$(nproc)" -ge "${#scenarios[@]}" ]; then
    pids=()
    for sc in "${scenarios[@]}"; do
        mkdir -p "$OUT/$sc"
        run_scenario "$sc" > "$OUT/$sc/console.txt" 2>&1 &
        pids+=($!)
    done
    for i in "${!scenarios[@]}"; do
        st=0; wait "${pids[$i]}" || st=$?
        echo "== $CAM / ${scenarios[$i]} (parallel)"; cat "$OUT/${scenarios[$i]}/console.txt"
        report "${scenarios[$i]}" "$st" ""
    done
else
    for sc in "${scenarios[@]}"; do
        echo "== $CAM / $sc"
        st=0; run_scenario "$sc" || st=$?
        report "$sc" "$st" ""
    done
fi
# One more try, alone, for a scenario the emulator never got through; a second timeout fails the job.
again=(${retry[@]+"${retry[@]}"}); retry=()
for sc in ${again[@]+"${again[@]}"}; do
    echo "== $CAM / $sc (retry after an infrastructure timeout, alone)"
    mv "$OUT/$sc" "$OUT/$sc.infra-timeout"
    st=0; run_scenario "$sc" || st=$?
    report "$sc" "$st" " (retry)"
done
if [ "${#retry[@]}" -gt 0 ]; then
    echo "INFRASTRUCTURE TIMEOUT twice: ${retry[*]} -- no functional result for them"; rc=1
fi

# Shooting scenarios (ci/qemu/shoot.py; needs a qemu-eos image with photo capture emulation): pictures taken by
# Canon and by ML (intervalometer, HDR bracketing, bulb), checked on the card. SHOOT=0 skips them. ML scenarios
# use the photo build where there is one, else the full build. Results: $OUT/shoot-<scenario>/, $OUT/shooting.md.
if [ "${SHOOT:-1}" = 1 ]; then
    shoot_zip=${ML_ZIP_PHOTO:-${ML_ZIP_FULL:-}}
    shoot=
    for sc in $SHOOT_SCENARIOS; do     # ML scenarios (ml-*) need a build
        case $sc in ml-*) [ -n "$shoot_zip" ] || continue ;; esac
        shoot+="$sc "
    done
    : > "$OUT/shooting.md"
    for sc in $shoot; do
        echo "== $CAM / shoot $sc"
        for attempt in 1 2; do       # one more try, alone, after an infrastructure timeout
            st=0
            python3 -I "$HERE/shoot.py" --cam "$CAM" --scenario "$sc" --rom-dir "$ML_ROM_DIR" \
                --base-sd "$ROOT/platform/sd.qcow2.xz" ${shoot_zip:+--zip "$shoot_zip"} \
                ${debugmsg:+--debugmsg-addr "$debugmsg"} --out "$OUT/shoot-$sc" | tee "$work/shoot.txt" || st=$?
            [ "$st" = 3 ] && [ "$attempt" = 1 ] && { echo "shoot $sc: INFRASTRUCTURE TIMEOUT, retrying"; continue; }
            break
        done
        grep -v '^ *$' "$work/shoot.txt" | grep -v 'qemu-system-arm' >> "$OUT/shooting.md" || true
        case $st in
            0) ;;
            3) echo "shoot $sc: INFRASTRUCTURE TIMEOUT twice -- no functional result"; rc=1 ;;
            *) echo "shoot $sc FAILED"; rc=1 ;;
        esac
    done
fi

# With the display-off segment, compare.py also checks the deep-sleep expectations (DEEP_SLEEP_ASSERT=0: report only)
[ "${DISPLAY_OFF_S%.*}" -gt 0 ] && export DEEP_SLEEP_ASSERT=${DEEP_SLEEP_ASSERT:-1}
python3 -I "$HERE/compare.py" "$CAM" "$OUT" "${scenarios[@]}" | tee "$OUT/comparison.md" || rc=1
if [ -s "$OUT/shooting.md" ]; then
    { echo; echo "## Shooting (ci/qemu/shoot.py)"; echo '```'; cat "$OUT/shooting.md"; echo '```'; } | tee -a "$OUT/comparison.md"
fi
exit $rc
