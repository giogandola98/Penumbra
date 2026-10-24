#!/usr/bin/env python3
"""Check that every module shipped in a platform's magiclantern.zip links against that camera's build.

ML links all enabled modules together at boot (TCC); one symbol missing from the camera's .sym makes the link
fail and *every* module shows "Err". This finds that at build time.

Usage: check_module_syms.py PLATFORM_DIR   (after `make -C PLATFORM_DIR`; needs arm-none-eabi-nm on PATH or in
       $ARM_BINPATH). Exit 1 if a shipped module imports a symbol the camera does not export.
"""
import glob
import os
import shutil
import subprocess
import sys


def main():
    plat = sys.argv[1]
    mods = os.path.join(plat, "build", "zip", "ML", "modules")
    syms = glob.glob(os.path.join(mods, "*.sym"))
    if len(syms) != 1:
        sys.exit("expected one .sym in %s, found %d" % (mods, len(syms)))
    nm = shutil.which("arm-none-eabi-nm") or os.path.join(os.environ.get("ARM_BINPATH", ""), "arm-none-eabi-nm")
    have = set()
    with open(syms[0]) as f:
        for line in f:
            parts = line.split()
            if len(parts) >= 2:
                have.add(parts[1])
    bad = 0
    for mo in sorted(glob.glob(os.path.join(mods, "*.mo"))):
        out = subprocess.run([nm, "-u", mo], capture_output=True, text=True, check=True).stdout.split()
        missing = sorted({w for w in out if w != "U" and w not in have})
        print("%-14s %s" % (os.path.basename(mo), "ok" if not missing else "MISSING: " + " ".join(missing)))
        bad += bool(missing)
    if bad:
        sys.exit(1)


if __name__ == "__main__":
    main()
