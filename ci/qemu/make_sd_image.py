#!/usr/bin/env python3
"""Build a raw SD card image for qemu-eos from ML's base image and (optionally) an ML build zip.

Replaces platform/create_disk_image_from_zip.py in CI: that script needs guestmount (FUSE + a libguestfs
appliance), which a confined rootless container cannot run. Here the qcow2 is converted to raw with
qemu-img and files are copied into the FAT partition with mtools, all as an unprivileged user.

Usage: make_sd_image.py BASE_QCOW2_XZ OUT_IMG [ML_ZIP]
Without ML_ZIP the card is the plain base image (Canon firmware only scenario).
"""
import lzma
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile


def fat_offset(img):
    """Byte offset of the first FAT partition (MBR), or 0 for a partitionless card."""
    with open(img, "rb") as f:
        mbr = f.read(512)
    if mbr[510:512] != b"\x55\xaa":
        raise SystemExit("no boot signature in %s" % img)
    # A FAT boot sector at LBA 0 (superfloppy) has a jump instruction; an MBR has a partition table.
    if mbr[0] in (0xEB, 0xE9) and (mbr[54:57] == b"FAT" or mbr[82:87] == b"FAT32"):
        return 0
    for i in range(4):
        entry = mbr[446 + 16 * i: 462 + 16 * i]
        ptype, lba = entry[4], struct.unpack_from("<I", entry, 8)[0]
        if ptype in (0x01, 0x04, 0x06, 0x0B, 0x0C, 0x0E) and lba:
            return lba * 512
    raise SystemExit("no FAT partition found in %s" % img)


def boot_flags(img, off):
    """Report the Canon card boot flags (EOS_DEVELOP / BOOTDISK) in the FAT boot sector."""
    with open(img, "rb") as f:
        f.seek(off)
        bs = f.read(512)
    return {"EOS_DEVELOP": b"EOS_DEVELOP" in bs, "BOOTDISK": b"BOOTDISK" in bs}


def main():
    if len(sys.argv) not in (3, 4):
        raise SystemExit(__doc__)
    base, out = sys.argv[1], sys.argv[2]
    ml_zip = sys.argv[3] if len(sys.argv) == 4 else None

    with tempfile.TemporaryDirectory() as tmp:
        qcow = os.path.join(tmp, "sd.qcow2")
        with lzma.open(base) as src, open(qcow, "wb") as dst:
            shutil.copyfileobj(src, dst)
        subprocess.run(["qemu-img", "convert", "-f", "qcow2", "-O", "raw", qcow, out], check=True)

        off = fat_offset(out)
        img = "%s@@%d" % (out, off)
        env = dict(os.environ, MTOOLS_SKIP_CHECK="1")
        if ml_zip:
            files = os.path.join(tmp, "files")
            with zipfile.ZipFile(ml_zip) as z:
                for n in z.namelist():
                    # Zip entries must stay inside the extraction dir.
                    if n.startswith("/") or ".." in n.split("/"):
                        raise SystemExit("unsafe path in zip: %s" % n)
                z.extractall(files)
            # Remove a previous ML install from the base image, then copy the new one.
            subprocess.run(["mdeltree", "-i", img, "::/ML"], env=env,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            subprocess.run(["mdel", "-i", img, "::/autoexec.bin"], env=env,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            entries = [os.path.join(files, e) for e in sorted(os.listdir(files))]
            subprocess.run(["mcopy", "-i", img, "-s", "-o", "-Q"] + entries + ["::/"],
                           env=env, check=True)

    flags = boot_flags(out, off)
    print("sd image: %s (FAT at offset %d, boot flags %s)" % (out, off, flags))
    listing = subprocess.run(["mdir", "-i", img, "-b", "::/"], env=env, check=True,
                             capture_output=True, text=True).stdout
    print(listing.strip())


if __name__ == "__main__":
    main()
