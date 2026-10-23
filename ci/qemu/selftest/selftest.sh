#!/bin/bash
# Self-test of the wakeup plugin and of the SD image tooling; needs no Canon ROM.
# Runs inside the ml-qemu image (CI) or anywhere with qemu-system-arm 4.2 + plugins, gcc,
# arm-none-eabi binutils, mtools and python3.
# Usage: ci/qemu/selftest/selftest.sh [OUT_DIR]   (default: emu-out/selftest)
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
OUT=${1:-$ROOT/emu-out/selftest}
rm -rf "$OUT" && mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)     # QEMU dlopen()s a plugin path without '/' from the library path

PLUGIN=$("$HERE/../build-plugin.sh" "$OUT")

AS=${AS:-arm-none-eabi-as}
LD=${LD:-arm-none-eabi-ld}
"$AS" -mcpu=arm926ej-s -o "$OUT/wfi_test.o" "$HERE/wfi_test.S"
"$LD" --no-warn-rwx-segments -T "$HERE/link.ld" -o "$OUT/wfi_test.elf" "$OUT/wfi_test.o"

# Generic ARMv5 machine: versatilepb has an ARM926 core, an SP804 timer and a PL190 VIC.
run_test() {   # $1 = plugin option (KEY=VALUE or empty), $2 = json path
    timeout 60 qemu-system-arm -M versatilepb -cpu arm926 -m 16M -nodefaults \
        -display none -monitor none -serial none -semihosting \
        -kernel "$OUT/wfi_test.elf" \
        -plugin "$PLUGIN,arg=out=$2${1:+,arg=$1}"
}

fail=0
check() {      # $1 = description, $2.. = command
    local what=$1; shift
    if "$@"; then echo "PASS: $what"; else echo "FAIL: $what"; fail=1; fi
}

echo "== run 1: full window, 50 ms intervals"
run_test "interval_ms=50" "$OUT/run1.json" > "$OUT/run1.log" 2>&1 && rc=0 || rc=$?
cat "$OUT/run1.log" "$OUT/run1.json" || true
check "guest exit code 0 (guest saw one IRQ per WFI)" test "$rc" -eq 0
check "plugin counts" python3 -I "$HERE/check.py" run1 "$OUT/run1.json"

echo "== run 2: window never reached (start_ms beyond the run)"
run_test "start_ms=600000" "$OUT/run2.json" > "$OUT/run2.log" 2>&1 && rc=0 || rc=$?
cat "$OUT/run2.json" || true
check "guest exit code 0" test "$rc" -eq 0
check "summary reports window not started" python3 -I "$HERE/check.py" run2 "$OUT/run2.json"

echo "== run 3: icount busy model"
timeout 60 qemu-system-arm -M versatilepb -cpu arm926 -m 16M -nodefaults \
    -display none -monitor none -serial none -semihosting -icount shift=4 \
    -kernel "$OUT/wfi_test.elf" -plugin "$PLUGIN,arg=out=$OUT/run3.json,arg=icount_shift=4" \
    > "$OUT/run3.log" 2>&1 && rc=0 || rc=$?
cat "$OUT/run3.log" "$OUT/run3.json" || true
check "guest exit code 0" test "$rc" -eq 0
check "icount counts" python3 -I "$HERE/check.py" run3 "$OUT/run3.json"

echo "== run 4: bad option is rejected"
if run_test "bogus=1" "$OUT/run4.json" > "$OUT/run4.log" 2>&1; then rc=0; else rc=$?; fi
check "QEMU refuses to start with a bad plugin option" test "$rc" -ne 0

echo "== SD image tooling (dummy ML zip, no ROM)"
check "make_sd_image.sh" "$HERE/test_sd_image.sh" "$OUT/sd"

if [ "$fail" -ne 0 ]; then echo "SELFTEST FAILED"; exit 1; fi
echo "SELFTEST PASSED"
