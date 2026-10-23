#!/usr/bin/env python3
"""Check a platform's stubs.S against a Canon ROM dump.

Each stub listed in <platform>/stubs-evidence.txt is checked against the evidence that was used
to find it (referenced strings, assert file/line, calls, literals, call sites), so that a typo or
a wrong pick in stubs.S fails CI instead of crashing the camera.

The ROM is read only from the path given on the command line; nothing ROM-derived is printed
except addresses. Without a ROM the check is skipped (exit 0), so it can run everywhere.

Usage: tools/check_stubs.py PLATFORM_DIR [ROM1.BIN]    (or ROM path in $ML_STUBS_ROM)

Evidence file format (one stub per line, '#' comments):
    rom BASE SIZE               ROM1 is mapped at BASE; only the first SIZE bytes are used
    copy SRC DST END            cstart copies ROM SRC to RAM DST..END (kernel code + data)
    <stub> <check> <check> ...
Checks (all must pass; NAME = another stub, VALUE = number or NAME[+off]):
    fn                function entry: first instruction stores LR (push / str lr,[sp,#-4]!) or is in a known
                      leaf form; anything else fails
    str=TEXT          the function references the NUL-terminated string TEXT (shell-style quotes allowed)
    strin=TEXT        the function references a string containing TEXT
    assert=FILE:LINE  the function passes line LINE and a file name ending in FILE to the assert handler
    call=VALUE        the function calls VALUE (BL/B)
    calledby=NAME     NAME calls this stub
    callers=N         at least N direct call sites in ROM + RAM copy
    lit=VALUE         the function loads the literal VALUE
    litin=VALUE[+-off] (data stubs) the function at VALUE loads this address (+-off)
    bl=VALUE          the stub itself is a BL instruction to VALUE (patch points)
    manual            no ROM check possible (e.g. MMIO); the evidence is in the stubs document
    word@ADDR         the 32-bit word at ADDR equals this stub's value
    blat@ADDR         the instruction at ADDR is a BL to this stub
The scan of a function stops at the next function entry (a push of LR after its first three
instructions) or after 0x2000 bytes.
"""
import os
import shlex
import re
import struct
import sys

SCAN_MAX = 0x2000


class Image:
    def __init__(self, rom, base, size, copy):
        if len(rom) < size:
            raise ValueError('ROM smaller than declared size')
        self.rom = rom[:size]
        self.base = base
        self.ram = b''
        self.ramlo = 0
        if copy:
            src, dst, end = copy
            o = src - base
            self.ram = self.rom[o:o + end - dst]
            self.ramlo = dst
        self._calls = None

    def r32(self, a):
        a &= 0xFFFFFFFF
        if self.base <= a <= self.base + len(self.rom) - 4:
            return struct.unpack_from('<I', self.rom, a - self.base)[0]
        # the flash is mirrored every len(rom) bytes up to the top of the address space
        if a >= self.base:
            m = self.base + (a - self.base) % len(self.rom)
            if m <= self.base + len(self.rom) - 4:
                return struct.unpack_from('<I', self.rom, m - self.base)[0]
        if self.ramlo <= a <= self.ramlo + len(self.ram) - 4:
            return struct.unpack_from('<I', self.ram, a - self.ramlo)[0]
        return None

    def cstr(self, a, n=256):
        out = bytearray()
        for i in range(n):
            w = self.r32((a + i) & ~3)
            if w is None:
                return None
            b = (w >> (8 * ((a + i) & 3))) & 0xFF
            if b == 0:
                return out.decode('latin1')
            out.append(b)
        return None

    @staticmethod
    def is_entry(w):
        # STMFD SP!,{..,LR} or STR LR,[SP,#-4]!
        return (w & 0xFFFF4000) == 0xE92D4000 or w == 0xE52DE004

    def func_words(self, f):
        """addresses of the function body: from f to the next entry, at most SCAN_MAX bytes"""
        out = []
        for a in range(f, f + SCAN_MAX, 4):
            w = self.r32(a)
            if w is None or (a >= f + 12 and self.is_entry(w)):
                break
            out.append(a)
        return out

    def branch_target(self, a):
        w = self.r32(a)
        if w is None or (w >> 25) & 7 != 5 or (w >> 28) == 0xF:
            return None
        off = w & 0xFFFFFF
        if off & 0x800000:
            off -= 0x1000000
        return (a + 8 + off * 4) & 0xFFFFFFFF

    def literal(self, a):
        """value loaded by LDR Rd,[PC,#imm] at a"""
        w = self.r32(a)
        if w is None or (w & 0x0F7F0000) != 0x051F0000:
            return None
        imm = w & 0xFFF
        return self.r32(a + 8 + (imm if w & 0x800000 else -imm))

    @staticmethod
    def rot_imm(w):
        imm, r = w & 0xFF, ((w >> 8) & 0xF) * 2
        return ((imm >> r) | (imm << (32 - r))) & 0xFFFFFFFF if r else imm

    def pc_rel(self, a):
        """address computed by ADD/SUB Rd,PC,#imm at a"""
        w = self.r32(a)
        if w is None:
            return None
        op = w & 0x0FFF0000
        if op == 0x028F0000:
            return a + 8 + self.rot_imm(w)
        if op == 0x024F0000:
            return a + 8 - self.rot_imm(w)
        return None

    def strings(self, f):
        out = []
        for a in self.func_words(f):
            for v in (self.literal(a), self.pc_rel(a)):
                if v is not None:
                    s = self.cstr(v)
                    if s:
                        out.append(s)
        return out

    def assert_lines(self, f):
        lines = set()
        for a in self.func_words(f):
            w = self.r32(a)
            if (w & 0x0FFFF000) == 0x03A02000:          # mov r2,#imm
                lines.add(self.rot_imm(w))
            elif (w & 0x0F7FF000) == 0x051F2000:        # ldr r2,=imm
                lines.add(self.literal(a))
        return lines

    def call_index(self):
        if self._calls is None:
            idx = {}
            for buf, base in ((self.rom, self.base), (self.ram, self.ramlo)):
                n = len(buf) // 4
                for i, w in enumerate(struct.unpack_from('<%dI' % n, buf)):
                    if (w >> 25) & 7 == 5 and (w >> 28) != 0xF:
                        off = w & 0xFFFFFF
                        if off & 0x800000:
                            off -= 0x1000000
                        t = (base + 4 * i + 8 + off * 4) & 0xFFFFFFFF
                        idx[t] = idx.get(t, 0) + 1
            self._calls = idx
        return self._calls


def parse_stubs(path):
    stubs = {}
    for line in open(path):
        line = line.split('//')[0]
        m = re.match(r'\s*(?:NSTUB|ARM32_FN|DATA_PTR)\(\s*(0x[0-9A-Fa-f]+)\s*,\s*([A-Za-z0-9_]+)\s*\)', line)
        if m:
            stubs[m.group(2)] = int(m.group(1), 16)
    return stubs


def value(tok, stubs):
    m = re.match(r'^([A-Za-z_][A-Za-z0-9_]*)?(?:([+-])(0x[0-9A-Fa-f]+|\d+))?$', tok)
    if tok.startswith('0x') or tok.isdigit():
        return int(tok, 0)
    if not m or m.group(1) not in stubs:
        raise KeyError(tok)
    v = stubs[m.group(1)]
    if m.group(2):
        d = int(m.group(3), 0)
        v = v + d if m.group(2) == '+' else v - d
    return v


def check(img, name, addr, chk, stubs):
    """returns None if OK, else a short reason"""
    if chk == 'manual':
        return None
    if chk == 'fn':
        w = img.r32(addr)
        return None if w is not None and img.is_entry(w) else 'no function entry'
    k, _, arg = chk.partition('=')
    if '@' in chk and not _:
        k, _, a = chk.partition('@')
        a = int(a, 16)
        if k == 'word':
            return None if img.r32(a) == addr else 'word@%x is not %x' % (a, addr)
        if k == 'blat':
            return None if img.branch_target(a) == addr else 'no BL at %x' % a
    if k in ('str', 'strin'):
        text = arg
        ss = img.strings(addr)
        ok = any(s == text for s in ss) if k == 'str' else any(text in s for s in ss)
        return None if ok else 'string not referenced'
    if k == 'assert':
        f, _, ln = arg.rpartition(':')
        ok = int(ln) in img.assert_lines(addr) and any(s.endswith(f) for s in img.strings(addr))
        return None if ok else 'assert %s not found' % arg
    if k == 'call':
        t = value(arg, stubs)
        return None if any(img.branch_target(a) == t for a in img.func_words(addr)) else 'no call to %s' % arg
    if k == 'calledby':
        c = value(arg, stubs)
        return None if any(img.branch_target(a) == addr for a in img.func_words(c)) else 'not called by %s' % arg
    if k == 'callers':
        n = img.call_index().get(addr, 0)
        return None if n >= int(arg) else 'only %d call sites' % n
    if k == 'lit':
        v = value(arg, stubs)
        return None if any(img.literal(a) == v for a in img.func_words(addr)) else 'literal %s not loaded' % arg
    if k == 'bl':
        return None if img.branch_target(addr) == value(arg, stubs) else 'not a BL to %s' % arg
    if k == 'litin':
        m = re.match(r'^(.+?)([+-](?:0x[0-9A-Fa-f]+|\d+))?$', arg)
        fn = m.group(1)
        f = value(fn, stubs)
        want = (addr + (int(m.group(2), 0) if m.group(2) else 0)) & 0xFFFFFFFF
        return None if any(img.literal(a) == want for a in img.func_words(f)) else 'not loaded by %s' % fn
    return 'unknown check %s' % chk


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    plat = argv[1].rstrip('/')
    rom_path = argv[2] if len(argv) > 2 else os.environ.get('ML_STUBS_ROM', '')
    ev_path = os.path.join(plat, 'stubs-evidence.txt')
    if not os.path.exists(ev_path):
        print('SKIPPED: no %s' % ev_path)
        return 0
    if not rom_path or not os.path.isfile(rom_path):
        print('SKIPPED: no ROM given for %s' % os.path.basename(plat))
        return 0
    stubs = parse_stubs(os.path.join(plat, 'stubs.S'))
    base = size = None
    copy = None
    entries = []
    for ln in open(ev_path):
        ln = shlex.split(ln, comments=True)
        if not ln:
            continue
        if ln[0] == 'rom':
            base, size = int(ln[1], 16), int(ln[2], 16)
        elif ln[0] == 'copy':
            copy = tuple(int(x, 16) for x in ln[1:4])
        else:
            entries.append((ln[0], ln[1:]))
    with open(rom_path, 'rb') as f:
        rom = f.read()
    img = Image(rom, base, size, copy)
    fails = 0
    checked = 0
    for name, checks in entries:
        if name not in stubs:
            print('FAIL %-36s not in stubs.S' % name)
            fails += 1
            continue
        addr = stubs[name]
        bad = []
        for c in checks:
            try:
                r = check(img, name, addr, c, stubs)
            except KeyError as e:
                r = 'unknown stub %s' % e
            if r:
                bad.append(r)
        checked += 1
        if bad:
            fails += 1
            print('FAIL %-36s 0x%08X: %s' % (name, addr, '; '.join(bad)))
    missing = sorted(set(stubs) - set(n for n, _ in entries) - {'firmware_entry'})
    print('%s: %d stubs checked, %d failed, %d in stubs.S without evidence%s' % (
        os.path.basename(plat), checked, fails, len(missing), (': ' + ' '.join(missing)) if missing else ''))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
