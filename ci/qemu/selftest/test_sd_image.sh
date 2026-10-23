#!/bin/bash
# make_sd_image.py with a dummy ML zip: files land on the card and read back identical.
# Usage: test_sd_image.sh OUT_DIR
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
OUT=${1:?usage: test_sd_image.sh OUT_DIR}
rm -rf "$OUT" && mkdir -p "$OUT"
export MTOOLS_SKIP_CHECK=1

python3 -I - "$OUT/dummy.zip" <<'PY'
import sys, zipfile
with zipfile.ZipFile(sys.argv[1], "w") as z:
    z.writestr("autoexec.bin", b"\x00dummy-autoexec" * 100)
    z.writestr("ML/modules/dummy.mo", b"module")
    z.writestr("ML/README", b"readme")
PY

python3 -I "$HERE/../make_sd_image.py" "$ROOT/platform/sd.qcow2.xz" "$OUT/sd.img" "$OUT/dummy.zip" | tee "$OUT/make.log"

off=$(sed -n 's/.*FAT at offset \([0-9]*\).*/\1/p' "$OUT/make.log")
mtype -i "$OUT/sd.img@@$off" ::/autoexec.bin > "$OUT/autoexec.readback"
python3 -I - "$OUT/autoexec.readback" <<'PY'
import sys
data = open(sys.argv[1], "rb").read()
assert data == b"\x00dummy-autoexec" * 100, "autoexec.bin read back differs"
PY
mdir -i "$OUT/sd.img@@$off" -b ::/ML/modules | grep -qi 'dummy.mo'
grep -q "'EOS_DEVELOP': True" "$OUT/make.log" || echo "note: base image has no EOS_DEVELOP flag"

# Canon-only card: base image without ML.
python3 -I "$HERE/../make_sd_image.py" "$ROOT/platform/sd.qcow2.xz" "$OUT/plain.img" > /dev/null
rm -f "$OUT"/*.img "$OUT/autoexec.readback"
echo "sd image tooling OK"
