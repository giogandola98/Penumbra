#!/usr/bin/env python3
"""Build-time safety check of the EOS M capture kit (ML_PROFILE=capture).

Fails if the linked binary could reach a Canon routine listed in the forbidden list, or if the zip ships more
than autoexec.bin and ML data. Checked:
  - every 32-bit word of the loaded sections (literal pools, function pointers, tables): no forbidden address,
    ROM addresses also in their 0xF8..0xFF aliases. Exception: words inside a `*_sigs` object, the read-only
    signature tables the kit uses to verify the firmware before reading from it (listed as "allowed");
  - every ARM B/BL/BLX target in executable sections: no forbidden address;
  - the ELF symbol table: no symbol whose value is a forbidden address, and no ROM symbol (a stub) named like
    a forbidden routine (ML's own logging no-ops of the same name live in RAM and are allowed);
  - every NUL-terminated string: no forbidden eventproc name (call() looks eventprocs up by name);
  - the zip: autoexec.bin plus files under ML/, no FIR, no modules.
Usage: check_capture_kit.py --elf ELF --autoexec BIN --zip ZIP --forbidden LIST
"""
import argparse
import re
import struct
import sys
import zipfile

SHT_SYMTAB, SHT_NOBITS = 2, 8
SHF_ALLOC, SHF_EXECINSTR = 0x2, 0x4


def load_forbidden(path):
    """[(addr, name, is_eventproc)] from '<address> <name> [reason]' lines."""
    out = []
    for line in open(path):
        line = line.split('#', 1)[0].strip()
        if not line:
            continue
        f = line.split()
        out.append((int(f[0], 16), f[1], 'eventproc' in f[2:]))
    return out


def aliases(addr):
    """ROM1 is visible in 16 MiB windows from 0xF8000000 up; RAM addresses only as themselves."""
    if addr >= 0xF8000000:
        low = addr & 0x00FFFFFF
        return {(w << 24) | low for w in range(0xF8, 0x100)}
    return {addr}


def read_elf(path):
    d = open(path, 'rb').read()
    if d[:4] != b'\x7fELF' or d[4] != 1 or d[5] != 1:
        sys.exit('not a 32-bit little-endian ELF: %s' % path)
    shoff, = struct.unpack_from('<I', d, 0x20)
    shentsize, shnum = struct.unpack_from('<HH', d, 0x2E)
    secs = []
    for i in range(shnum):
        name, typ, flags, addr, off, size, link, info, align, entsize = \
            struct.unpack_from('<10I', d, shoff + i * shentsize)
        secs.append(dict(type=typ, flags=flags, addr=addr, off=off, size=size, link=link, entsize=entsize))
    loaded = [(s['addr'], d[s['off']:s['off'] + s['size']], bool(s['flags'] & SHF_EXECINSTR))
              for s in secs if s['flags'] & SHF_ALLOC and s['type'] != SHT_NOBITS and s['size']]
    syms = []
    for s in secs:
        if s['type'] != SHT_SYMTAB:
            continue
        strs = secs[s['link']]
        for j in range(s['size'] // 16):
            nm, val, size, info, other, shndx = struct.unpack_from('<IIIBBH', d, s['off'] + j * 16)
            end = d.index(b'\0', strs['off'] + nm)
            syms.append((d[strs['off'] + nm:end].decode('latin-1'), val, size))
    return loaded, syms


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--elf', required=True)
    ap.add_argument('--autoexec', required=True)
    ap.add_argument('--zip', required=True)
    ap.add_argument('--forbidden', required=True)
    a = ap.parse_args()

    forbidden = load_forbidden(a.forbidden)
    by_addr = {}
    for addr, name, _ in forbidden:
        for al in aliases(addr):
            by_addr[al] = name
    names = {n for _, n, _ in forbidden} | {n.lstrip('_') for _, n, _ in forbidden}
    procs = {n.encode() for _, n, ep in forbidden if ep}

    loaded, syms = read_elf(a.elf)
    sig_ranges = [(v, v + n) for name, v, n in syms if name.endswith('_sigs') and n]
    hits, allowed = [], []
    words = branches = 0
    for base, data, exe in loaded:
        for off in range(0, len(data) - 3, 4):
            w, = struct.unpack_from('<I', data, off)
            words += 1
            if w in by_addr:
                at = base + off
                if any(lo <= at < hi for lo, hi in sig_ranges):
                    allowed.append('word 0x%08X at 0x%08X -> %s (signature table, read only)' % (w, at, by_addr[w]))
                    continue
                hits.append('word 0x%08X at 0x%08X -> %s' % (w, base + off, by_addr[w]))
            if exe and (w >> 25) & 7 == 5:          # B/BL (cond != 0xF) and BLX imm (cond == 0xF)
                imm = w & 0xFFFFFF
                imm = imm - (1 << 24) if imm & 0x800000 else imm
                tgt = (base + off + 8 + (imm << 2) + ((w >> 23) & 2 if w >> 28 == 0xF else 0)) & 0xFFFFFFFF
                branches += 1
                if tgt in by_addr:
                    hits.append('branch at 0x%08X -> 0x%08X %s' % (base + off, tgt, by_addr[tgt]))
        for m in re.finditer(rb'(?<![\x20-\x7e])([\x20-\x7e]{2,})\x00', data):
            if m.group(1) in procs:
                hits.append('string "%s" at 0x%08X (eventproc name)' % (m.group(1).decode(), base + m.start(1)))
    for name, val, _ in syms:
        if name and val >= 0xF8000000 and (name in names or name.lstrip('_') in names):
            hits.append('symbol %s = 0x%08X' % (name, val))
        if val in by_addr and name:
            hits.append('symbol %s = 0x%08X (%s)' % (name, val, by_addr[val]))

    bad = []
    with zipfile.ZipFile(a.zip) as z:
        entries = [n for n in z.namelist() if not n.endswith('/')]
    for n in entries:
        if n == 'autoexec.bin':
            continue
        if not n.startswith('ML/') or n.upper().endswith('.FIR') or n.startswith('ML/modules/'):
            bad.append(n)
    if 'autoexec.bin' not in entries:
        bad.append('(autoexec.bin missing)')

    print('capture kit check: %d forbidden entries, %d words, %d branches, %d symbols, %d zip files'
          % (len(forbidden), words, branches, len(syms), len(entries)))
    for h in allowed:
        print('allowed: ' + h)
    for h in hits:
        print('FORBIDDEN: ' + h)
    for n in bad:
        print('ZIP: not allowed: ' + n)
    if hits or bad:
        print('capture kit check FAILED')
        return 1
    print('capture kit check OK')
    return 0


if __name__ == '__main__':
    sys.exit(main())
