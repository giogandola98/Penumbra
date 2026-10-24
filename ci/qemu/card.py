#!/usr/bin/env python3
"""Read the emulated SD card (same as bench/card.py in QEMU-EOS-Optimized): files on a FAT12/16/32 image
(first MBR partition) and the exposure tags (EXIF) of the pictures on it. No dependencies, so it runs in
the CI image as is.

    card.py SD.img                 list files (size, path)
    card.py SD.img --json          files + exposure of DCIM pictures, as JSON
    card.py SD.img --get PATH OUT  extract one file
"""
import json
import struct
import sys


class Fat:
    def __init__(self, path):
        self.img = open(path, "rb").read()
        img = self.img
        start = 0
        if img[510:512] == b"\x55\xaa" and img[446 + 4] != 0:
            start = struct.unpack("<I", img[446 + 8:446 + 12])[0] * 512
        bs = img[start:start + 512]
        bps, spc, rsv, nfat, nroot, tot16, _, fsz16 = struct.unpack("<HBHBHHBH", bs[11:24])
        tot = tot16 or struct.unpack("<I", bs[32:36])[0]
        fsz = fsz16 or struct.unpack("<I", bs[36:40])[0]
        self.fat = start + rsv * bps
        root = self.fat + nfat * fsz * bps
        root_secs = (nroot * 32 + bps - 1) // bps
        self.data = root + root_secs * bps
        nclus = (tot - rsv - nfat * fsz - root_secs) // spc
        self.type = 12 if nclus < 4085 else 16 if nclus < 65525 else 32
        self.csize = spc * bps
        if self.type == 32:
            self.root = self.chain_data(struct.unpack("<I", bs[44:48])[0])
        else:
            self.root = img[root:root + nroot * 32]

    def next(self, c):
        img, f = self.img, self.fat
        if self.type == 16:
            return struct.unpack("<H", img[f + 2 * c:f + 2 * c + 2])[0]
        if self.type == 32:
            return struct.unpack("<I", img[f + 4 * c:f + 4 * c + 4])[0] & 0x0FFFFFFF
        v = struct.unpack("<H", img[f + c * 3 // 2:f + c * 3 // 2 + 2])[0]
        return v >> 4 if c & 1 else v & 0xFFF

    def chain_data(self, c, size=None):
        end = {12: 0xFF8, 16: 0xFFF8, 32: 0x0FFFFFF8}[self.type]
        out, seen = [], set()
        while 2 <= c < end and c not in seen:
            seen.add(c)
            o = self.data + (c - 2) * self.csize
            out.append(self.img[o:o + self.csize])
            c = self.next(c)
        b = b"".join(out)
        return b if size is None else b[:size]

    def walk(self, raw=None, path=""):
        raw = self.root if raw is None else raw
        for i in range(0, len(raw), 32):
            e = raw[i:i + 32]
            if not e or e[0] == 0:
                break
            if e[0] == 0xE5 or e[11] == 0x0F or e[11] & 0x08:
                continue
            name = e[0:8].decode("latin1").rstrip()
            ext = e[8:11].decode("latin1").rstrip()
            name += "." + ext if ext else ""
            if name in (".", ".."):
                continue
            clus = struct.unpack("<H", e[26:28])[0]
            if self.type == 32:
                clus |= struct.unpack("<H", e[20:22])[0] << 16
            size = struct.unpack("<I", e[28:32])[0]
            p = path + "/" + name
            if e[11] & 0x10:
                yield p + "/", 0, clus
                yield from self.walk(self.chain_data(clus), p)
            else:
                yield p, size, clus

    def files(self):
        return [(p, s) for p, s, _ in self.walk() if not p.endswith("/")]

    def read(self, path):
        for p, size, clus in self.walk():
            if p.upper() == path.upper():
                return self.chain_data(clus, size)
        raise FileNotFoundError(path)


EXIF_TAGS = {0x0110: "model", 0x829A: "exposure_time", 0x829D: "f_number", 0x8827: "iso",
             0x9204: "exposure_bias", 0x8822: "exposure_program"}


def _tiff(buf, base=0):
    bo = "<" if buf[base:base + 2] == b"II" else ">"

    def u(fmt, o):
        return struct.unpack(bo + fmt, buf[base + o:base + o + struct.calcsize(fmt)])[0]

    out = {}

    def ifd(off, depth):
        n = u("H", off)
        for i in range(n):
            e = off + 2 + 12 * i
            tag, typ, cnt = u("H", e), u("H", e + 2), u("I", e + 4)
            size = {1: 1, 2: 1, 3: 2, 4: 4, 5: 8, 7: 1, 9: 4, 10: 8}.get(typ, 1) * cnt
            val = u("I", e + 8) if size > 4 else e + 8
            if tag == 0x8769 and depth < 2:         # EXIF sub-IFD
                ifd(u("I", e + 8), depth + 1)
            if tag not in EXIF_TAGS or EXIF_TAGS[tag] in out:
                continue
            if typ == 2:
                v = buf[base + val:base + val + cnt].split(b"\0")[0].decode("latin1")
            elif typ == 3:
                v = u("H", val)
            elif typ == 4:
                v = u("I", val)
            elif typ in (5, 10):
                f = "I" if typ == 5 else "i"
                num, den = u(f, val), u(f, val + 4)
                v = "%d/%d" % (num, den)
            else:
                continue
            out[EXIF_TAGS[tag]] = v

    ifd(u("I", 4), 0)
    return out


def exif(data):
    """Exposure tags of a CR2 (TIFF) or JPEG (APP1 Exif) file."""
    try:
        if data[:2] in (b"II", b"MM"):
            return _tiff(data)
        o = 2
        while o + 4 <= len(data) and data[o] == 0xFF:
            marker, ln = data[o + 1], struct.unpack(">H", data[o + 2:o + 4])[0]
            if marker == 0xE1 and data[o + 4:o + 10] == b"Exif\0\0":
                return _tiff(data, o + 10)
            if marker == 0xDA:
                break
            o += 2 + ln
    except (struct.error, IndexError, UnicodeDecodeError):
        pass
    return {}


def pictures(path):
    """[{path, size, exif}] for the pictures in DCIM, in file order."""
    fs = Fat(path)
    out = []
    for p, size in fs.files():
        if p.upper().startswith("/DCIM/") and p.upper().rsplit(".", 1)[-1] in ("JPG", "CR2"):
            out.append({"path": p, "size": size, "exif": exif(fs.read(p))})
    return out


if __name__ == "__main__":
    if len(sys.argv) >= 4 and sys.argv[2] == "--get":
        open(sys.argv[4], "wb").write(Fat(sys.argv[1]).read(sys.argv[3]))
    elif len(sys.argv) >= 3 and sys.argv[2] == "--json":
        print(json.dumps({"files": Fat(sys.argv[1]).files(), "pictures": pictures(sys.argv[1])}, indent=1))
    else:
        for p, s in Fat(sys.argv[1]).files():
            print("%10d %s" % (s, p))
