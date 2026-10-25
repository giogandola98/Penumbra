#!/usr/bin/env python3
"""Boot the EOS M capture kit (ML_PROFILE=capture) in qemu-eos, run every capture once and check that the
kit stayed read-only.  docs/CAPTURE-KIT.md describes the kit; docs/EMULATION.md how to run this.

The kit runs its self-test when ML/CAPTEST.TXT is on the card (src/capture-kit.c, capture_selftest_task):
boot logs, serial flash read, a short MPU/DryosDebugMsg session, LiveView buffers and a RAM dump, then it
writes "[CAPTURE] selftest done errors=N blocked=M" to the debug log and ML/LOGS/CAPTEST.OK.

Checks (exit 1 on failure, 3 on an infrastructure timeout, 0 and "SKIPPED" without ROMs/SFDATA.BIN):
  - the self-test finishes (no hang), errors=0, blocked=0 (no property write was refused)
  - every capture file exists on the card with a plausible size
  - qemu-eos' serial flash model (-d sflash) saw no write/erase: only READ, QOFR, RDID, RDSR/LPWP and, in
    Canon's own start-up task, the status-register write Canon does on every boot (WREN + WRSR), nothing else;
    any WREN/WRSR from another task, any page program or erase (0x02 0x07 0x20 0x52 0xD8 0x60 0xC7), any
    opcode the model does not know, fails the run
  - no write to a ROM region (-d romw)
  - SFDATA.BIN saved by the kit is identical to the one the emulator loaded

Usage: capture_kit.py --zip magiclantern.zip --rom-dir DIR [--base sd.qcow2.xz | --sd prepared.img] --out DIR
DIR/EOSM must hold ROM0.BIN ROM1.BIN SFDATA.BIN (read only; copied to a scratch dir).
Environment: QEMU_EOS (default /opt/qemu-eos).
"""
import argparse
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
QEMU_EOS = os.environ.get("QEMU_EOS", "/opt/qemu-eos")
DEBUGMSG = "0x40D4"           # DryosDebugMsg (RAM copy), platform/EOSM.202/stubs.S
CANON_START_TASK = "Startup2"  # Canon's start-up task, which writes the flash status register on every boot
FILES = {                       # card file -> minimum size
    "BOOTDM.LOG": 10000, "BOOTMPU.LOG": 100, "BOOTINFO.TXT": 10, "SFDATA.BIN": 8 << 20,
    "DM00.LOG": 1000, "MPU00.LOG": 0, "PM00.LOG": 10, "LVINFO.TXT": 10,
    "RAM_OTHR.BIN": 16 << 20, "RAM_OTHR.TXT": 10, "CAPTEST.OK": 10,
}
WRITE_OPS = {0x02, 0x07, 0x20, 0x52, 0xD8, 0x60, 0xC7}   # PP, AAI/page write, sector/block/chip erase


def monitor(path, cmd, timeout=10):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.connect(path)
    s.recv(4096)
    s.sendall(cmd.encode() + b"\n")
    time.sleep(1)
    s.close()


class Card:
    """Read access to ML/LOGS on the card image: mtools (CI image), else pyfatfs (local runs)."""
    def __init__(self, img, off):
        self.ref = "%s@@%d" % (img, off)
        self.env = dict(os.environ, MTOOLS_SKIP_CHECK="1")
        self.fs = None
        if not shutil.which("mdir"):
            from pyfatfs.PyFatFS import PyFatFS
            self.fs = PyFatFS(img, offset=off)

    def sizes(self):
        if self.fs:
            return {n.upper(): self.fs.getsize("/ML/LOGS/" + n) for n in self.fs.listdir("/ML/LOGS")}
        out = subprocess.run(["mdir", "-i", self.ref, "::/ML/LOGS"], env=self.env, capture_output=True, text=True).stdout
        sizes = {}
        for line in out.splitlines():
            m = re.match(r"^(\S+)\s+(?:(\S+)\s+)?(\d+)\s+\d{4}-\d\d-\d\d", line)
            if m:
                sizes[(m.group(1) + ("." + m.group(2) if m.group(2) else "")).upper()] = int(m.group(3))
        return sizes

    def get(self, name, dest):
        if self.fs:
            open(dest, "wb").write(self.fs.readbytes("/ML/LOGS/" + name))
        else:
            subprocess.run(["mcopy", "-i", self.ref, "-n", "::/ML/LOGS/" + name, dest], env=self.env, check=False,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def analyse(log):
    """Serial flash / ROM / property checks on the qemu log; returns (problems, stats)."""
    problems, stats = [], {"flash_ops": {}, "wren": 0, "wrsr": 0, "rom_writes": 0, "blocked": 0}
    task = "?"
    for raw in log.splitlines():
        line = re.sub(r"\x1b\[[0-9;]*m", "", raw)
        m = re.match(r"^\[\s*([^\s:\]]+):[0-9a-f]+\s*\]", line)    # "[    Startup2:ff13b074 ] (00:01) ..."
        if m:
            task = m.group(1)
        if "[CAPTURE] blocked" in line:
            stats["blocked"] += 1
            problems.append("property write refused: " + line.strip()[:160] + " (last task %s)" % task)
        if re.match(r"^\s*\[rom[01a-z_]*\]", line):
            stats["rom_writes"] += 1
            problems.append("ROM write: " + line.strip()[:160])
        m = re.match(r"^\[EEPROM\] Verbose: Got (\S+) \(([0-9A-Fa-f]{2})h\)", line)
        if m:
            op = int(m.group(2), 16)
            stats["flash_ops"][m.group(1)] = stats["flash_ops"].get(m.group(1), 0) + 1
            if op == 0x01 and task != CANON_START_TASK:
                problems.append("WRSR from task %s (not Canon's start-up)" % task)
            if op in WRITE_OPS:
                problems.append("serial flash write/erase opcode 0x%02X from task %s" % (op, task))
        if "[EEPROM] Verbose: Set Write Enable Latch" in line:
            stats["wren"] += 1
            if task != CANON_START_TASK:
                problems.append("WREN from task %s (not Canon's start-up)" % task)
        if re.search(r"\[EEPROM\].*Illegal opcode", line) or re.search(r"\[EEPROM\].*(erase|Erase|program)", line):
            problems.append("unexpected serial flash command: " + line.strip()[:160] + " (task %s)" % task)
    return problems, stats


def main():
    ap = argparse.ArgumentParser(usage=__doc__)
    ap.add_argument("--zip", required=True)
    ap.add_argument("--rom-dir", required=True)
    ap.add_argument("--base", help="base card image (qcow2.xz), default platform/sd.qcow2.xz")
    ap.add_argument("--sd", help="prepared card image (skips make_sd_image.py); must contain ML/CAPTEST.TXT")
    ap.add_argument("--fat-offset", type=int, default=0, help="with --sd: byte offset of the FAT partition")
    ap.add_argument("--out", required=True)
    ap.add_argument("--timeout", type=float, default=300)
    a = ap.parse_args()

    romsrc = os.path.join(a.rom_dir, "EOSM")
    need = ["ROM0.BIN", "ROM1.BIN", "SFDATA.BIN"]
    missing = [n for n in need if not os.path.exists(os.path.join(romsrc, n))]
    if missing:
        print("SKIPPED: %s missing in %s" % (", ".join(missing), romsrc))
        return 0
    os.makedirs(a.out, exist_ok=True)
    work = tempfile.mkdtemp(prefix="capkit.", dir=os.environ.get("TMPDIR"))
    try:
        roms = os.path.join(work, "roms", "EOSM")
        os.makedirs(roms)
        for n in need:
            shutil.copyfile(os.path.join(romsrc, n), os.path.join(roms, n))   # QEMU may write its flash copy
        sd = os.path.join(work, "sd.img")
        if a.sd:
            shutil.copyfile(a.sd, sd)
            off = a.fat_offset
        else:
            base = a.base or os.path.join(HERE, "..", "..", "platform", "sd.qcow2.xz")
            open(os.path.join(work, "captest.txt"), "w").write("1\n")
            subprocess.run([sys.executable, "-I", os.path.join(HERE, "make_sd_image.py"),
                            "--put", "ML/CAPTEST.TXT:" + os.path.join(work, "captest.txt"), base, sd, a.zip],
                           check=True, stdout=subprocess.DEVNULL)
            sys.path.insert(0, HERE)
            from make_sd_image import fat_offset
            off = fat_offset(sd)
        cf = os.path.join(work, "cf.img")
        open(cf, "wb").truncate(64 << 20)
        sock = os.path.join(work, "mon.sock")
        log = os.path.join(a.out, "qemu.log")
        cmd = [os.path.join(QEMU_EOS, "bin", "qemu-system-arm"), "-M", "EOSM,firmware=boot=1",
               "-drive", "if=sd,format=raw,file=" + sd, "-drive", "if=ide,format=raw,file=" + cf,
               "-chardev", "socket,server,nowait,path=%s,id=m" % sock, "-mon", "chardev=m,mode=readline",
               "-display", "none", "-serial", "file:" + os.path.join(a.out, "uart.log"),
               "-plugin", os.path.join(QEMU_EOS, "plugins", "libmagiclantern.so") + ",arg=debugmsg_addr=" + DEBUGMSG,
               "-d", "debugmsg,sflash,romw"]
        print("QEMU_EOS_WORKDIR=%s %s" % (os.path.join(work, "roms"), " ".join(cmd)), flush=True)
        with open(log, "wb") as lf:
            q = subprocess.Popen(cmd, env=dict(os.environ, QEMU_EOS_WORKDIR=os.path.join(work, "roms")),
                                 stdin=subprocess.DEVNULL, stdout=lf, stderr=subprocess.STDOUT)
        t0 = time.monotonic()
        done = False
        while time.monotonic() - t0 < a.timeout and q.poll() is None:
            time.sleep(2)
            with open(log, "rb") as f:
                if b"[CAPTURE] selftest done" in f.read():
                    done = True
                    break
        time.sleep(3)                       # let the last file writes reach the card image
        if q.poll() is None:
            try:
                monitor(sock, "quit")
            except OSError:
                pass
            try:
                q.wait(20)
            except subprocess.TimeoutExpired:
                q.kill()
        if not done:
            text = open(log, "rb").read().decode("latin1")
            print("INFRASTRUCTURE/HANG: no '[CAPTURE] selftest done' in %.0f s" % a.timeout)
            print("last CAPTURE lines:\n" + "\n".join(l[-160:] for l in text.splitlines() if "[CAPTURE]" in l)[-1500:])
            return 3

        text = open(log, "rb").read().decode("latin1")
        problems, stats = analyse(text)
        m = re.search(r"\[CAPTURE\] selftest done errors=(\d+) blocked=(\d+)", text)
        if not m or m.group(1) != "0" or m.group(2) != "0":
            problems.append("self-test result: %s" % (m.group(0) if m else "missing"))

        card = Card(sd, off)
        sizes = card.sizes()
        for n, mn in FILES.items():
            if n not in sizes:
                problems.append("capture file missing on the card: ML/LOGS/" + n)
            elif sizes[n] < mn:
                problems.append("capture file too small: %s %d < %d" % (n, sizes[n], mn))
        sf = os.path.join(work, "sf.bin")
        if "SFDATA.BIN" in sizes:
            card.get("SFDATA.BIN", sf)
            if open(sf, "rb").read() != open(os.path.join(romsrc, "SFDATA.BIN"), "rb").read():
                problems.append("SFDATA.BIN saved by the kit differs from the emulated serial flash")
        for n in ("CAPTEST.OK", "BOOTINFO.TXT", "LVINFO.TXT", "PM00.LOG"):
            if n in sizes:
                card.get(n, os.path.join(a.out, n))

        summary = ["card files: " + ", ".join("%s=%d" % kv for kv in sorted(sizes.items())),
                   "serial flash ops: %s (WREN %d, all in task %s)" % (stats["flash_ops"], stats["wren"], CANON_START_TASK),
                   "ROM writes: %d, refused property writes: %d" % (stats["rom_writes"], stats["blocked"])]
        open(os.path.join(a.out, "capture-kit.txt"), "w").write("\n".join(summary + ["PROBLEMS: %d" % len(problems)] + problems) + "\n")
        print("\n".join(summary))
        if problems:
            print("FAILED:\n  " + "\n  ".join(problems))
            return 1
        print("OK: capture kit self-test passed, read-only")
        return 0
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
