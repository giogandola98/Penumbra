#!/bin/bash
# CI host-side driver for the camera scenarios (Forgejo job `emulation`, runs on the runner host).
# For each camera in EMU_CAMS: fetch its ROMs into a private tmpfs dir, build the ML zips in the
# ml-ci image, run ci/qemu/run_cam.sh in the qemu-eos image with the ROMs mounted read-only,
# then delete the ROMs. Cameras without ROMs are reported as SKIPPED; the script then exits 0.
#
# Usage (from the job workspace): src-checkout/ci/qemu/ci-emulation.sh
# Needs: QEMU_IMAGE, CI_IMAGE (image:tag, built from CI_CONTAINERFILE if missing), SANDBOX (podman flags),
#        ROM_TOKEN and/or JOB_TOKEN. EMU_CAMS selects cameras (default "1100D EOSM").
set -euo pipefail
SRC=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
EMU_CAMS=${EMU_CAMS:-1100D EOSM}
OUT=$SRC/emu-out
mkdir -p "$OUT"

# ROMs live only in RAM, readable only by the job user, and are removed whatever happens.
romroot=$(mktemp -d /dev/shm/ml-roms.XXXXXX)
chmod 700 "$romroot"
cleanup() { chmod -R u+w "$romroot" 2>/dev/null || true; rm -rf "$romroot"; }
trap cleanup EXIT INT TERM

build_zip() {   # $1 platform dir, $2 profile (or "" for platforms without profiles), $3 output zip, $4 extra make vars
    local prof=${2:+ML_PROFILE=$2}
    podman run $SANDBOX -v "$SRC:/work" -w /work "$CI_IMAGE" bash -c \
        "make -C platform/$1 clean >/dev/null && make -C platform/$1 -j\$(nproc) $prof ${4:-} >/dev/null && cp platform/$1/build/magiclantern.zip /work/$3"
}

rc=0
for cam in $EMU_CAMS; do
    echo "=== $cam"
    st=0
    ROM_TOKEN=${ROM_TOKEN:-} JOB_TOKEN=${JOB_TOKEN:-} "$SRC/ci/qemu/fetch_roms.sh" "$cam" "$romroot" || st=$?
    if [ "$st" -eq 3 ]; then
        mkdir -p "$OUT/$cam"
        echo "SKIPPED: no ROM for $cam" | tee "$OUT/$cam/comparison.md"
        continue
    elif [ "$st" -ne 0 ]; then
        echo "ROM fetch for $cam failed"; rc=1; continue
    fi

    if ! podman image exists "$CI_IMAGE"; then
        podman build --network=host -q -t "$CI_IMAGE" -f "$CI_CONTAINERFILE" "$(dirname "$CI_CONTAINERFILE")" >/dev/null
    fi
    plat=$(cd "$SRC/platform" && ls -d "$cam".[0-9]* | head -1)
    mkdir -p "$SRC/emu-zips"
    envs=(-e "CAM=$cam" -e "WARMUP_S=${WARMUP_S:-40}" -e "MEASURE_S=${MEASURE_S:-30}")
    if grep -q '^ML_PROFILE' "$SRC/platform/$plat/Makefile"; then
        build_zip "$plat" full "emu-zips/$cam-full.zip" && build_zip "$plat" photo "emu-zips/$cam-photo.zip" || { rc=1; continue; }
        envs+=(-e "ML_ZIP_FULL=/work/emu-zips/$cam-full.zip" -e "ML_ZIP_PHOTO=/work/emu-zips/$cam-photo.zip")
    else
        # Same build with the counted powersave holds (docs/analysis/powersave-hold.md);
        # the EOSM photo profile already has them.
        build_zip "$plat" "" "emu-zips/$cam-full.zip" && build_zip "$plat" "" "emu-zips/$cam-hold.zip" POWERSAVE_HOLD=y \
            || { rc=1; continue; }
        envs+=(-e "ML_ZIP_FULL=/work/emu-zips/$cam-full.zip" -e "ML_ZIP_HOLD=/work/emu-zips/$cam-hold.zip")
    fi

    podman run $SANDBOX --tmpfs /tmp:rw,size=2g -v "$SRC:/work" -v "$romroot:/roms:ro" -w /work \
        "${envs[@]}" -e ML_ROM_DIR=/roms -e "OUT=/work/emu-out/$cam" "$QEMU_IMAGE" ci/qemu/run_cam.sh || rc=1
    chmod -R u+w "$romroot/$cam"; rm -rf "${romroot:?}/$cam"
done

# Never let anything ROM-sized or disk-image-like reach the artifacts.
find "$OUT" -type f \( -iname '*.bin' -o -iname '*.img' -o -iname '*.qcow2' -o -size +20M \) -print -delete
exit $rc
