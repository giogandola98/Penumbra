#!/usr/bin/env python3
"""Count ARM instructions executed per frame, original loops vs overlay kernels.

Runs arm_driver.elf (camera compiler and flags) on an emulated ARM926 with
Unicorn and reports, for each kernel, the instructions and memory accesses
executed on one EOSM LCD frame, and whether both versions produced the same
output checksum. Instruction counts ignore caches and pipeline interlocks, but
on an in-order, single-issue ARM926 they track run time closely.

usage: armcount.py arm_driver.elf  (needs arm-none-eabi-{nm,objcopy,objdump} on PATH
       and the 'unicorn' Python package)
"""
import re
import subprocess
import sys
import tempfile

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_BLOCK
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_CPU_ARM_926

RAM = 32 << 20
LOAD_ADDR = 0x10000   # -Ttext in the Makefile


def tool(name, *args):
    return subprocess.run(["arm-none-eabi-" + name, *args], check=True, capture_output=True, text=True).stdout


def main(elf):
    syms = {}
    for line in tool("nm", elf).splitlines():
        parts = line.split()
        if len(parts) == 3:
            syms[parts[2]] = int(parts[0], 16)

    # memory accesses per instruction address
    mem_ops = {}
    for line in tool("objdump", "-d", elf).splitlines():
        m = re.match(r"\s*([0-9a-f]+):\s+[0-9a-f]{8}\s+(\S+)", line)
        if m:
            mn = m.group(2)
            mem_ops[int(m.group(1), 16)] = 1 if re.match(r"(ldr|str|ldm|stm|push|pop)", mn) else 0

    with tempfile.NamedTemporaryFile(suffix=".bin") as f:
        tool("objcopy", "-O", "binary", elf, f.name)
        image = open(f.name, "rb").read()
    base = LOAD_ADDR

    uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    uc.ctl_set_cpu_model(UC_CPU_ARM_926)
    uc.mem_map(0, RAM)
    uc.mem_write(base, image)

    stats = {"insn": 0, "mem": 0}
    block_cache = {}

    def on_block(uc_, addr, size, _):
        c = block_cache.get(addr)
        if c is None:
            c = (size // 4, sum(mem_ops.get(a, 0) for a in range(addr, addr + size, 4)))
            block_cache[addr] = c
        stats["insn"] += c[0]
        stats["mem"] += c[1]

    uc.hook_add(UC_HOOK_BLOCK, on_block)
    halt = syms["halt"]

    def call(fn, *args):
        regs = [UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2]
        for r, v in zip(regs, args):
            uc.reg_write(r, v & 0xFFFFFFFF)
        uc.reg_write(UC_ARM_REG_SP, RAM - 64)
        uc.reg_write(UC_ARM_REG_LR, halt)
        stats["insn"] = stats["mem"] = 0
        uc.emu_start(syms[fn], halt)
        return uc.reg_read(UC_ARM_REG_R0), stats["insn"], stats["mem"]

    call("setup")

    cases = []
    # focus peaking at the threshold where ~1% of the LiveView samples are hits,
    # which is what the auto-threshold in zebra.c converges to
    samples = ((720 - 16) // 2) * ((480 - 16 + 2) // 3)
    for fe in (0, 1, 2):
        lo, hi = 1, 2048
        while lo < hi:
            mid = (lo + hi) // 2
            if call("ref_peak_count", mid, fe)[0] > samples // 100:
                lo = mid + 1
            else:
                hi = mid
        cases.append((f"peaking LV fe={fe} (thr {lo})", "ref_peak", "new_peak", (lo, fe)))
    print("ARM926 (Unicorn), EOSM LCD 720x480 photo-like frame, per frame")
    print(f"  {'kernel':30s} {'insn orig':>11s} {'insn new':>11s} {'ratio':>6s}   {'mem orig':>9s} {'mem new':>9s}  exact")
    cases += [
        ("zebras luma (1st frame)", "ref_zebra", "new_zebra", (0, 95, 5)),
        ("zebras luma (steady)", "ref_zebra", "new_zebra", (0, 95, 5)),
        ("zebras rgb (1st frame)", "ref_zebra", "new_zebra", (1, 95, 5)),
        ("zebras rgb (steady)", "ref_zebra", "new_zebra", (1, 95, 5)),
        ("histogram luma", "ref_hist", "new_hist", (0,)),
        ("histogram rgb", "ref_hist", "new_hist", (1,)),
    ]
    ok = True
    for label, ref, new, args in cases:
        h_ref, i_ref, m_ref = call(ref, *args)
        h_new, i_new, m_new = call(new, *args)
        if "zebra" in ref:
            # the checksum loop over the overlay is not part of the kernel: measure it alone
            _, i_hash, m_hash = call("hash_only")
            i_ref -= i_hash; i_new -= i_hash; m_ref -= m_hash; m_new -= m_hash
        same = h_ref == h_new
        ok &= same
        print(f"  {label:30s} {i_ref:11d} {i_new:11d} {i_ref / i_new:6.2f}   {m_ref:9d} {m_new:9d}  {'yes' if same else 'NO'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
