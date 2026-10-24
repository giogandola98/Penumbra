#!/usr/bin/env python3
"""Boot one Canon camera scenario in qemu-eos headless and measure it.

Timeline (host time, which QEMU's virtual clock follows without -icount). Every phase is anchored to
what the emulator has actually done, not to a fixed delay from launch, so a loaded runner (slow QEMU
start, slow boot, slow monitor) shifts the timeline instead of measuring the wrong thing:
  t0                 QEMU's monitor socket appears (QEMU is up, plugins loaded)
  ready              the guest reached its idle state: every --ready-marker seen in the DryosDebugMsg
                     log (e.g. Canon's display on, ML's boot hold released), else a stable screen
  window start       max(warmup, ready + settle) after t0; the window then lasts --measure seconds:
                     wakeups plugin rates, task switches from `-d tasks`, monitor PC / counter samples
  end                screenshot via the monitor, then `quit` (the plugin writes its summary at exit)

Then, optionally (--display-off-s, 1100D): the display-off idle segment (deep sleep, see
docs/analysis/powersave-hold.md) timed from the display actually turning off, a key to wake the
camera (the screen must change), and for ML builds the ML menu and a config save.

If the emulator never gets somewhere (no monitor, no ready marker, monitor starved, screendump or
samples missing) the run ends with "INFRASTRUCTURE TIMEOUT" and exit code 3: no functional verdict,
and never a pass. Functional check failures exit 1.

Writes into OUT_DIR: qemu.log (DryosDebugMsg + task switches + QEMU messages), wakeups.json,
tasks.json (task switches per task inside the window), boot.png / idle.png, result.json.
"""
import argparse
import hashlib
import json
import os
import re
import socket
import subprocess
import sys
import time

QEMU_EOS = os.environ.get("QEMU_EOS", "/opt/qemu-eos")
EXIT_INFRA = 3


class InfraTimeout(Exception):
    """The emulator did not get where the timeline needs it in time: no functional verdict possible."""


def _monitor_once(path, cmd, timeout):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    try:
        s.connect(path)
        buf = b""
        while b"(qemu)" not in buf:               # banner + prompt: QEMU accepted this client
            chunk = s.recv(4096)                  # socket.timeout (an OSError) if QEMU is starved
            if not chunk:
                raise ConnectionError("monitor closed before its prompt")
            buf += chunk
        s.sendall(cmd.encode() + b"\n")
        deadline = time.monotonic() + timeout
        try:
            while time.monotonic() < deadline and buf.count(b"(qemu)") < 2:
                chunk = s.recv(4096)
                if not chunk:
                    break
                buf += chunk
        except socket.timeout:
            pass                                  # the command was sent; its output may be partial
        return buf.decode(errors="replace")
    finally:
        s.close()


def monitor_cmd(path, cmd, timeout=10.0, tries=4):
    """Send one HMP command over the monitor socket and return what QEMU printed back.
    The command is only sent once QEMU's prompt arrived; connecting is retried (QEMU serves one monitor
    client at a time and is slow to accept on a loaded host). InfraTimeout if it never answers."""
    last = None
    for i in range(tries):
        try:
            return _monitor_once(path, cmd, timeout)
        except OSError as e:
            last = e
            time.sleep(min(2.0, 0.25 * 2 ** i))
    raise InfraTimeout("QEMU monitor did not answer '%s' (%d tries, %.0f s each): %s" % (cmd, tries, timeout, last))


def screenshot(mon, out_png, wait_s=20.0):
    """Screendump to out_png; None if QEMU never produced it (the caller decides whether that matters)."""
    ppm = out_png[:-4] + ".ppm"
    from PIL import Image
    for attempt in range(2):
        try:
            os.remove(ppm)
        except FileNotFoundError:
            pass
        try:
            monitor_cmd(mon, "screendump " + ppm)
        except InfraTimeout as e:
            print("screenshot %s: %s" % (os.path.basename(out_png), e), file=sys.stderr)
            continue
        deadline = time.monotonic() + wait_s       # screendump completes asynchronously
        while time.monotonic() < deadline:
            if os.path.exists(ppm) and os.path.getsize(ppm) > 0:
                try:
                    with Image.open(ppm) as im:
                        im.load()                 # fails while the file is still being written
                        im.save(out_png)
                    os.remove(ppm)
                    return out_png
                except Exception:
                    pass
            time.sleep(0.2)
        print("screenshot %s: no complete screendump after %.0f s (attempt %d)" % (
            os.path.basename(out_png), wait_s, attempt + 1), file=sys.stderr)
    return None


def frame_hash(png):
    """SHA-256 of the pixels (not of the PNG file), to tell whether the screen changed."""
    if not png:
        return None
    try:
        from PIL import Image
        with Image.open(png) as im:
            return hashlib.sha256(im.convert("RGB").tobytes()).hexdigest()[:16]
    except Exception:
        return None


def frame_uniform(png):
    try:
        from PIL import Image
        with Image.open(png) as im:
            lo, hi = zip(*im.convert("RGB").getextrema())
            return lo == hi
    except Exception:
        return True


class LogTail:
    """Incremental search of QEMU's log (DryosDebugMsg lines) from a byte offset."""

    def __init__(self, path, start=0):
        self.path, self.pos, self.tail = path, start, ""

    def find(self, pattern):
        """True once `pattern` appeared after the start offset."""
        with open(self.path, "rb") as f:
            f.seek(self.pos)
            data = f.read()
        self.pos += len(data)
        text = self.tail + re.sub(r"\x1b\[[0-9;]*m", "", data.decode(errors="replace"))
        if re.search(pattern, text):
            return True
        self.tail = text[-512:]
        return False


def wait_log(log, patterns, since, timeout, q):
    """Seconds until every regex in `patterns` appeared in the log after byte offset `since`, None on timeout."""
    t = time.monotonic()
    pending = {p: LogTail(log, since) for p in patterns}
    while True:
        for p in list(pending):
            if pending[p].find(p):
                del pending[p]
        if not pending:
            return time.monotonic() - t
        if q.poll() is not None:
            raise RuntimeError("QEMU exited with code %s (see qemu.log)" % q.returncode)
        if time.monotonic() - t > timeout:
            return None
        time.sleep(0.5)


def wait_screen(mon, out_png, differs_from, min_s, timeout, q):
    """Screenshot after at least min_s, then again until the frame differs from `differs_from` (a hash)
    or `timeout` passed. Returns the last screenshot path (None if screendump never worked)."""
    time.sleep(min_s)
    t = time.monotonic()
    while True:
        png = screenshot(mon, out_png)
        if differs_from is None or (png and frame_hash(png) != differs_from) or time.monotonic() - t > timeout:
            return png
        if q.poll() is not None:
            return png
        time.sleep(1)


def sample(mon, end, pm_addr):
    """Until monotonic time `end`: guest PC and Canon's power-save counter every ~0.5 s via the monitor.
    Returns (pcs, ctr, attempts); a starved monitor shows up as attempts >> samples."""
    pcs, ctr = {}, {}
    attempts = 0
    while time.monotonic() < end - 0.5:
        attempts += 1
        try:
            m = re.search(r"R15=([0-9a-f]{8})", monitor_cmd(mon, "info registers", timeout=5, tries=2))
            if m:
                pcs[m.group(1)] = pcs.get(m.group(1), 0) + 1
            if pm_addr:
                m = re.search(r":\s*0x([0-9a-f]+)", monitor_cmd(mon, "xp /1wx " + pm_addr, timeout=5, tries=2))
                if m:
                    v = str(int(m.group(1), 16))
                    ctr[v] = ctr.get(v, 0) + 1
        except InfraTimeout as e:
            print("sample: %s" % e, file=sys.stderr)
        time.sleep(min(0.5, max(0, end - time.monotonic())))
    time.sleep(max(0, end - time.monotonic()))
    return pcs, ctr, attempts


def enough_samples(n, seconds):
    """A monitor sample is taken every ~0.5 s; fewer than a quarter of those means the monitor was starved."""
    return n >= max(5, int(seconds / 0.5 / 4))


def top(pcs, n=10):
    tot = sum(pcs.values())
    return [("0x" + pc, round(c / tot, 3)) for pc, c in sorted(pcs.items(), key=lambda kv: -kv[1])[:n]] if tot else []


def read_log(path, start, end):
    with open(path, "rb") as f:
        f.seek(start)
        data = f.read(max(0, end - start)).decode(errors="replace")
    return re.sub(r"\x1b\[[0-9;]*m", "", data)


def plugin_window(records, t0_ms, t1_ms):
    """Wakeups-plugin rates over [t0_ms, t1_ms] (plugin time) from its interval records, each record
    weighted by its overlap with the window."""
    acc = {"seconds": 0.0, "wfi": 0.0, "irq": 0.0, "insns": 0.0, "halted_ms": 0.0}
    for r in records:
        a, b = max(t0_ms, r["t_start_ms"]), min(t1_ms, r["t_end_ms"])
        if b <= a or r["t_end_ms"] <= r["t_start_ms"]:
            continue
        f = (b - a) / (r["t_end_ms"] - r["t_start_ms"])
        acc["seconds"] += (b - a) / 1000.0
        for k in ("wfi", "irq", "insns", "halted_ms"):
            acc[k] += f * (r.get(k) or 0)
    s = acc["seconds"]
    if s <= 0:
        return None
    return {"seconds": round(s, 2), "wakeups_per_s": round(acc["wfi"] / s, 2), "irq_per_s": round(acc["irq"] / s, 2),
            "insns_per_wakeup": round(acc["insns"] / acc["wfi"], 1) if acc["wfi"] else 0,
            "mips": round(acc["insns"] / s / 1e6, 3), "halted_s": round(acc["halted_ms"] / 1000.0, 2),
            "busy_ratio": round(max(0.0, 1 - acc["halted_ms"] / 1000.0 / s), 4)}


SR_RE = re.compile(r"pmSelfRefresh\s*:\s*(In|Out)")
QSR_RE = re.compile(r"\[PM1100D\] self-refresh #(\d+) out after ([0-9.]+) s, wake irq (0x[0-9A-Fa-f]+)")


def pm_segment(text, seconds, tasks):
    """Deep-sleep evidence in one log segment: Canon's own pmSelfRefresh messages, the emulator's
    self-refresh lines (length of each sleep, wake interrupt), power-save counter and ML holds."""
    sr = SR_RE.findall(text)
    q = QSR_RE.findall(text)
    pm = re.findall(r"\[PM\] (?:Disable|Enable)PowerSave \(Counter = (\d+)\)", text)
    ps = re.findall(r"\[PS\] (?:hold|release) .*? -> (\d+)", text)
    return {
        "self_refresh_in": sr.count("In"), "self_refresh_out": sr.count("Out"),
        "qemu_self_refresh": len(q), "qemu_self_refresh_s": round(sum(float(x[1]) for x in q), 3),
        "qemu_self_refresh_max_s": max([float(x[1]) for x in q] or [0]),
        "wake_irqs": sorted(set(x[2] for x in q)),
        "self_refresh_in_progress": len(re.findall(r"\[PM1100D\] self-refresh #\d+ in", text)) > len(q),
        "pm_calls": len(pm), "pm_counter_last": int(pm[-1]) if pm else None,
        "ps_log": [l.strip()[-80:] for l in text.splitlines() if "[PS] " in l][:10],
        "task_switches_per_s": round(sum(tasks.values()) / seconds, 2) if seconds else None,
        "top_tasks": sorted(tasks.items(), key=lambda kv: -kv[1])[:8],
    }


TASK_RE = re.compile(r"Task switch\S*\s+to\s+(\S+?):")


def count_task_switches(log_path, start, end):
    """Task switches per task name between two byte offsets of the QEMU log."""
    counts = {}
    with open(log_path, "rb") as f:
        f.seek(start)
        data = f.read(max(0, end - start)).decode(errors="replace")
    # qemu-eos colours its log; strip ANSI escapes before matching.
    data = re.sub(r"\x1b\[[0-9;]*m", "", data)
    for m in TASK_RE.finditer(data):
        counts[m.group(1)] = counts.get(m.group(1), 0) + 1
    return counts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cam", required=True)
    ap.add_argument("--rom-dir", required=True, help="dir containing CAM/ROM0.BIN ...")
    ap.add_argument("--sd", required=True)
    ap.add_argument("--cf", required=True)
    ap.add_argument("--boot", action="store_true", help="set the camera bootflag (run autoexec.bin)")
    ap.add_argument("--plugin", required=True, help="path to libwakeups.so")
    ap.add_argument("--debugmsg-addr", default="", help="DryosDebugMsg address from stubs.S")
    ap.add_argument("--tasks", action="store_true", help="log task switches (-d tasks)")
    ap.add_argument("--pm-counter-addr", default="",
                    help="RAM address of Canon's power-save counter (1100D: 0x2B64), sampled in the window")
    ap.add_argument("--menu-s", type=float, default=6,
                    help="ML scenarios: seconds with the ML menu open after the window ([PM] calls counted)")
    ap.add_argument("--display-off-s", type=float, default=0,
                    help="after the window: turn the display off (--display-key), idle this long, then wake "
                         "the camera with the same key (0 = skip). docs/analysis/powersave-hold.md")
    ap.add_argument("--display-key", default="i", help="qemu-eos key that turns the display off (1100D: i = DISP)")
    ap.add_argument("--config-check", action="store_true",
                    help="ML scenarios: after the menu, change the selected entry (SET) and close the menu, "
                         "which makes ML save its config")
    ap.add_argument("--no-menu", action="store_true", help="ML scenarios: do not open the ML menu")
    ap.add_argument("--active-hold", default="",
                    help="ML scenarios: this ML hold stays taken for the whole run (e.g. intervalometer), "
                         "so ML's holds are expected >= 1 instead of 0")
    ap.add_argument("--ready-marker", action="append", default=[],
                    help="regex that the DryosDebugMsg log must show before the window can start (repeatable; "
                         "e.g. Canon's display on, ML's boot hold released). Without one (or without "
                         "--debugmsg-addr): two identical, non-blank screenshots in a row")
    ap.add_argument("--settle", type=float, default=10,
                    help="the window starts at least this long after the guest was ready")
    ap.add_argument("--start-timeout", type=float, default=180, help="QEMU monitor must appear within this")
    ap.add_argument("--ready-timeout", type=float, default=300,
                    help="the guest must be ready within this many seconds of QEMU being up")
    ap.add_argument("--step-timeout", type=float, default=60,
                    help="display off / wake / menu: how long to wait for the guest to react to a key")
    ap.add_argument("--display-off-marker", default="TurnOffDisplay",
                    help="log regex showing the display-off key took effect (the idle segment is timed from it)")
    ap.add_argument("--menu-tour", default="",
                    help="ML scenarios: comma-separated qemu-eos keys sent with the ML menu reopened after the "
                         "config check (e.g. right,right = next tabs); each must change the screen "
                         "(menu_tourNN_<key>.png), then MENU closes it")
    ap.add_argument("--expect-modules", default="",
                    help="ML scenarios: comma-separated modules that must load at boot ('[MODULE] loaded <name>' "
                         "in the log, no '[MODULE] link failed')")
    ap.add_argument("--probe-keys", default="",
                    help="comma-separated qemu-eos keys sent at the end; logs the GUI events each one causes")
    ap.add_argument("--warmup", type=float, default=40)
    ap.add_argument("--measure", type=float, default=30)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    os.makedirs(a.out, exist_ok=True)
    mon = os.path.join(a.out, "qemu.monitor")
    log = os.path.join(a.out, "qemu.log")
    wj = os.path.join(a.out, "wakeups.json")

    # 1 s interval records: the measurement window and the display-off segment are cut out of them
    plugin = "%s,arg=out=%s,arg=start_ms=%d,arg=interval_ms=%d" % (
        a.plugin, wj, int(a.warmup * 1000), 1000)
    cmd = [os.path.join(QEMU_EOS, "bin", "qemu-system-arm"),
           "-M", "%s,firmware=boot=%d" % (a.cam, 1 if a.boot else 0),
           "-drive", "if=sd,format=raw,file=" + a.sd,
           "-drive", "if=ide,format=raw,file=" + a.cf,
           "-chardev", "socket,server,nowait,path=%s,id=monsock" % mon,
           "-mon", "chardev=monsock,mode=readline",
           "-display", "none", "-serial", "file:" + os.path.join(a.out, "uart.log"),
           "-name", a.cam,
           "-plugin", plugin]
    d_args = []
    if a.debugmsg_addr:
        cmd += ["-plugin", os.path.join(QEMU_EOS, "plugins", "libmagiclantern.so")
                + ",arg=debugmsg_addr=" + a.debugmsg_addr]
        d_args.append("debugmsg")
    if a.tasks:
        d_args.append("tasks")
    if d_args:
        cmd += ["-d", ",".join(d_args)]

    env = dict(os.environ, QEMU_EOS_WORKDIR=a.rom_dir)
    print("QEMU_EOS_WORKDIR=%s %s" % (a.rom_dir, " ".join(cmd)), flush=True)
    t0 = time.monotonic()
    with open(log, "wb") as logf:
        q = subprocess.Popen(cmd, env=env, stdin=subprocess.DEVNULL, stdout=logf, stderr=subprocess.STDOUT)
    result = {"cam": a.cam, "boot_ml": a.boot, "warmup_s": a.warmup, "measure_s": a.measure}
    log_start = log_end = 0
    menu_start = menu_end = 0
    win0 = a.warmup                                # window start, seconds after t0 (plugin time)
    t_quit = None
    try:
        while not os.path.exists(mon) and q.poll() is None and time.monotonic() - t0 < a.start_timeout:
            time.sleep(0.1)
        if q.poll() is not None:
            raise RuntimeError("QEMU exited early with code %s (see qemu.log)" % q.returncode)
        if not os.path.exists(mon):
            raise InfraTimeout("QEMU monitor did not appear within %.0f s of launch" % a.start_timeout)
        # The timeline starts when QEMU is up, not at Popen: QEMU loads its plugins (whose window is
        # timed from their load) just before it creates the monitor socket, and a cold start of the
        # binary on a busy runner took 10-30 s.
        result["qemu_start_s"] = round(time.monotonic() - t0, 2)
        t0 = time.monotonic()

        # Wait for the guest to be idle, however long the boot takes on this host: a fixed delay
        # measured half a boot on a loaded runner (no ML yet at the window end, holds still taken).
        markers = a.ready_marker if a.debugmsg_addr else []
        if markers:
            if wait_log(log, markers, 0, a.ready_timeout, q) is None:
                missing = [m for m in markers if not LogTail(log).find(m)]
                raise InfraTimeout("guest not ready %.0f s after QEMU start: no %s in the log" % (
                    a.ready_timeout, " / ".join("'%s'" % m for m in missing)))
        else:
            prev = None
            while True:
                png = screenshot(mon, os.path.join(a.out, "ready.png"))
                h = frame_hash(png)
                if h and h == prev and not frame_uniform(png):
                    break
                prev = h
                if time.monotonic() - t0 > a.ready_timeout:
                    raise InfraTimeout("screen not stable %.0f s after QEMU start" % a.ready_timeout)
                time.sleep(2)
            try:
                os.remove(os.path.join(a.out, "ready.png"))
            except FileNotFoundError:
                pass
        result["ready_s"] = round(time.monotonic() - t0, 2)
        win0 = max(a.warmup, result["ready_s"] + a.settle)
        result["window_start_s"] = round(win0, 2)
        if win0 > a.warmup:
            print("guest ready after %.1f s: window starts at %.1f s instead of %.0f s" % (
                result["ready_s"], win0, a.warmup), flush=True)

        time.sleep(max(0, win0 - (time.monotonic() - t0)))
        log_start = os.path.getsize(log)
        result["boot_png"] = screenshot(mon, os.path.join(a.out, "boot.png"))
        # Sample the guest PC through the monitor during the window: a ROM-independent profile that
        # shows where the CPU spends its time (e.g. an idle loop that spins instead of sleeping).
        # Canon's power-save counter is sampled the same way: a cross-check of the exact count of its
        # changes, which comes from the [PM] messages Canon logs on every Disable/EnablePowerSave.
        pcs, ctr, tries = sample(mon, t0 + win0 + a.measure, a.pm_counter_addr)
        result["pc_samples"] = sum(pcs.values())
        result["sample_attempts"] = tries
        result["top_pcs"] = top(pcs)
        if a.pm_counter_addr:
            result["pm_counter_samples"] = ctr
        log_end = os.path.getsize(log)
        result["idle_png"] = screenshot(mon, os.path.join(a.out, "idle.png"))
        if not enough_samples(result["pc_samples"], a.measure) or (
                a.pm_counter_addr and not enough_samples(sum(ctr.values()), a.measure)):
            raise InfraTimeout("monitor starved in the window: %d PC / %d counter samples of %d attempts" % (
                result["pc_samples"], sum(ctr.values()), tries))
        if not (result["boot_png"] and result["idle_png"]):
            raise InfraTimeout("screendump did not complete (boot.png %s, idle.png %s)" % (
                bool(result["boot_png"]), bool(result["idle_png"])))
        if a.display_off_s > 0:
            # Display off and idle: Canon's PowerMgr may enter selfRefresh (all clocks off, DryOS
            # timers stopped) once the image engine clock is off and nothing restamped it for ~1 s.
            # Then the same key wakes the camera; the screen must change (GUI alive after the sleep).
            # The segment is timed from the display really turning off (its log line), not from the key.
            seg = {"key": a.display_key, "seconds": a.display_off_s}
            seg_log0 = os.path.getsize(log)
            idle_hash = frame_hash(result["idle_png"])
            monitor_cmd(mon, "sendkey " + a.display_key)
            if a.debugmsg_addr and a.display_off_marker:
                ack = wait_log(log, [a.display_off_marker], seg_log0, a.step_timeout, q)
                seg["display_off_ack_s"] = None if ack is None else round(ack, 2)
            seg["t0"] = round(time.monotonic() - t0, 2)
            dpcs, dctr, dtries = sample(mon, time.monotonic() + a.display_off_s, a.pm_counter_addr)
            seg["t1"] = round(time.monotonic() - t0, 2)
            seg_log1 = os.path.getsize(log)
            seg["top_pcs"] = top(dpcs, 5)
            seg["pc_samples"] = sum(dpcs.values())
            seg["pm_counter_samples"] = dctr
            if not enough_samples(seg["pc_samples"], a.display_off_s) or (
                    a.pm_counter_addr and not enough_samples(sum(dctr.values()), a.display_off_s)):
                raise InfraTimeout("monitor starved with the display off: %d PC / %d counter samples of %d" % (
                    seg["pc_samples"], sum(dctr.values()), dtries))
            seg["dark_png"] = screenshot(mon, os.path.join(a.out, "display_off.png"))
            if not seg["dark_png"]:
                raise InfraTimeout("screendump did not complete (display_off.png)")
            if seg["dark_png"] and frame_hash(seg["dark_png"]) == idle_hash:
                print("note: screen unchanged after the display-off key", file=sys.stderr)
            monitor_cmd(mon, "sendkey " + a.display_key)
            # At least 4 s as before, then until the screen changes (a slow host redraws late).
            seg["woken_png"] = wait_screen(mon, os.path.join(a.out, "woken.png"), frame_hash(seg["dark_png"]),
                                           4, a.step_timeout, q)
            if not seg["woken_png"]:
                raise InfraTimeout("screendump did not complete (woken.png)")
            seg_log2 = os.path.getsize(log)
            seg["log"] = [seg_log0, seg_log1, seg_log2]
            result["display_off"] = seg
        if a.boot and not a.no_menu:
            # After the window, so it cannot skew the metrics: open the ML menu (Trash = Delete key
            # in qemu-eos) and capture it.
            # The menu is a consumer of lens info outside LiveView: Canon's counter changes counted
            # while it is open show whether ML still refreshes it (docs/analysis/powersave-hold.md).
            menu_start = os.path.getsize(log)
            before = frame_hash(result.get("display_off", {}).get("woken_png") or result["idle_png"])
            tm = time.monotonic()
            monitor_cmd(mon, "sendkey delete")
            result["menu_png"] = wait_screen(mon, os.path.join(a.out, "menu.png"), before,
                                             max(3, a.menu_s), a.step_timeout, q)
            menu_end = os.path.getsize(log)
            result["menu_s"] = round(time.monotonic() - tm, 2)
            if not result["menu_png"]:
                raise InfraTimeout("screendump did not complete (menu.png)")
            # Close it again: ML's idle state must come back with no hold left (checked below).
            # On a fresh card the ML menu opens on its beta warning, which the first MENU only dismisses.
            # --config-check: then SET selects the first entry of the tab ML opens on (Help: its help
            # page; selecting an entry marks the config dirty), MENU leaves the help page and MENU closes
            # the menu, so ML autosaves its config: a file write after the deep sleep (checked on the card
            # by run_cam.sh, fio holds by compare.py). Otherwise MENU twice: the second closes the menu
            # (or, without the warning, opens Canon's menu, which ML does not hold). Trash would reopen it.
            keys = ["m", "spc", "m", "m"] if a.config_check else ["m", "m"]
            keys_log = os.path.getsize(log)
            for i, k in enumerate(keys):
                monitor_cmd(mon, "sendkey " + k)
                time.sleep(2)
                if i == 0:
                    # The menu itself (first tab), after the beta warning (if any) was dismissed.
                    result["menu_main_png"] = screenshot(mon, os.path.join(a.out, "menu_main.png"))
                if k == "spc":
                    result["menu_set_png"] = screenshot(mon, os.path.join(a.out, "menu_set.png"))
            time.sleep(3)
            if a.config_check and a.debugmsg_addr:
                # The config save is a card write: wait (bounded) for its FIO hold to be released.
                w = wait_log(log, [r"\[PS\] release fio"], keys_log, a.step_timeout, q)
                result["config_save_wait_s"] = None if w is None else round(w, 2)
            result["menu_closed_png"] = screenshot(mon, os.path.join(a.out, "menu_closed.png"))
            if a.menu_tour:
                # Menu navigation: reopen the ML menu (no beta warning any more: it opens on the tab it was
                # closed on) and walk it with the given keys (left/right outside edit mode switch tabs).
                # Every key must change the screen: a frozen menu or a dead key fails here. MENU closes it;
                # the holds checks below then also cover this walk.
                tour = []
                monitor_cmd(mon, "sendkey delete")
                prev = wait_screen(mon, os.path.join(a.out, "menu_tour00_open.png"),
                                   frame_hash(result["menu_closed_png"]), 2, a.step_timeout, q)
                tour.append({"key": "delete", "png": prev, "changed": bool(prev) and (
                    frame_hash(prev) != frame_hash(result["menu_closed_png"]))})
                for i, k in enumerate(a.menu_tour.split(",")):
                    monitor_cmd(mon, "sendkey " + k)
                    time.sleep(2)
                    png = screenshot(mon, os.path.join(a.out, "menu_tour%02d_%s.png" % (i + 1, k)))
                    tour.append({"key": k, "png": png, "changed": bool(png and prev) and (
                        frame_hash(png) != frame_hash(prev))})
                    prev = png or prev
                monitor_cmd(mon, "sendkey m")
                time.sleep(3)
                png = screenshot(mon, os.path.join(a.out, "menu_tour_closed.png"))
                tour.append({"key": "m", "png": png, "changed": bool(png and prev) and (
                    frame_hash(png) != frame_hash(prev))})
                if not all(t["png"] for t in tour):
                    raise InfraTimeout("screendump did not complete (menu tour)")
                result["menu_tour"] = tour
        if a.probe_keys:
            # Key probe (port bring-up): which GUI events Canon's firmware derives from each emulated
            # key. GUI_Control / GUI_OTHEREVENT debug messages name the event code ML's gui.h uses.
            probe = []
            for i, k in enumerate(a.probe_keys.split(",")):
                p0 = os.path.getsize(log)
                monitor_cmd(mon, "sendkey " + k)
                time.sleep(1.5)
                txt = read_log(log, p0, os.path.getsize(log))
                ev = [l.strip()[-80:] for l in txt.splitlines() if re.search(r"GUI_(Control|CONTROL|OTHEREVENT|ChangeMode)", l)]
                png = screenshot(mon, os.path.join(a.out, "key%02d_%s.png" % (i, k)))
                probe.append({"key": k, "events": ev[:8], "png": png})
            with open(os.path.join(a.out, "keys.json"), "w") as f:
                json.dump(probe, f, indent=1)
            result["key_probe"] = [(p["key"], p["events"][:2]) for p in probe]
        if q.poll() is not None:
            raise RuntimeError("QEMU died during the run, code %s (see qemu.log)" % q.returncode)
        t_quit = time.monotonic() - t0
        monitor_cmd(mon, "quit")
        q.wait(timeout=30)
    except InfraTimeout as e:
        result["infra_error"] = str(e)
        print("INFRASTRUCTURE TIMEOUT: %s" % e, file=sys.stderr)
    except (RuntimeError, OSError, subprocess.TimeoutExpired) as e:
        # Keep going: the log and whatever the plugin wrote are the evidence of what went wrong.
        result["error"] = str(e)
        print("ERROR: %s" % e, file=sys.stderr)
    finally:
        if q.poll() is None:
            q.kill()
            q.wait()
        try:
            os.remove(mon)
        except FileNotFoundError:
            pass

    summary = None
    intervals = []
    if os.path.exists(wj):
        for line in open(wj):
            rec = json.loads(line)
            if rec.get("type") == "summary":
                summary = rec
            elif rec.get("type") == "interval":
                intervals.append(rec)
    result["wakeups_total"] = summary
    # The measurement window only (the summary runs until quit, i.e. also covers what follows the window)
    win_stats = plugin_window(intervals, win0 * 1000, (win0 + a.measure) * 1000) if summary else None
    result["window_plugin_s"] = win_stats["seconds"] if win_stats else None
    result["wakeups"] = dict(summary, **win_stats) if win_stats else summary
    # The plugin times its window from its own load (CLOCK_MONOTONIC, like this script). If the two
    # clocks disagree by more than QEMU's start-up, the window was not where we meant it.
    if summary and summary.get("t_end_ms") and t_quit is not None:
        result["plugin_clock_lag_s"] = round(t_quit - summary["t_end_ms"] / 1000.0, 2)
    if a.tasks:
        tasks = count_task_switches(log, log_start, log_end)
        with open(os.path.join(a.out, "tasks.json"), "w") as f:
            json.dump(tasks, f, indent=1, sort_keys=True)
        result["task_switches"] = sum(tasks.values())
        result["task_switches_per_s"] = round(sum(tasks.values()) / a.measure, 2)
        result["top_tasks"] = sorted(tasks.items(), key=lambda kv: -kv[1])[:8]
    text = open(log, "rb").read().decode(errors="replace")
    if "Assertion" in text or "qemu-system-arm: " in text:
        tail = [l for l in text.splitlines() if "Assertion" in l or "MEMTX" in l or "qemu-system-arm:" in l]
        result["qemu_errors"] = tail[-5:]
    # ML's own DryOS tasks show up in the task-switch log once autoexec.bin runs.
    result["ml_detected"] = bool(re.search(r"\b(ml_init|menu_task|shoot_task|livev_hiprio_task|clock_task)\b", text))
    result["log_lines"] = text.count("\n")
    # CPU power save (docs/analysis/powersave-hold.md): Canon logs its counter on every
    # Disable/EnablePowerSave, ML logs its own holds as "[PS] hold|release <who> -> <total>".
    # Values at the end of the measurement window (the ML menu is opened after it).
    with open(log, "rb") as f:
        upto = re.sub(r"\x1b\[[0-9;]*m", "", f.read(log_end or None).decode(errors="replace"))
    with open(log, "rb") as f:
        f.seek(log_start)
        window = re.sub(r"\x1b\[[0-9;]*m", "", f.read(max(0, log_end - log_start)).decode(errors="replace"))
    pm_re = r"\[PM\] (?:Disable|Enable)PowerSave \(Counter = (\d+)\)"
    pm = re.findall(pm_re, upto)
    ps = re.findall(r"\[PS\] (?:hold|release) .*? -> (\d+)", upto)
    result["pm_counter_window_end"] = int(pm[-1]) if pm else None
    result["pm_calls_in_window"] = len(re.findall(pm_re, window))
    result["pm_changes_per_s"] = round(result["pm_calls_in_window"] / a.measure, 2)
    if menu_end > menu_start:
        with open(log, "rb") as f:
            f.seek(menu_start)
            menu = re.sub(r"\x1b\[[0-9;]*m", "", f.read(menu_end - menu_start).decode(errors="replace"))
        result["pm_calls_menu"] = len(re.findall(pm_re, menu))
    result["ml_holds_window_end"] = int(ps[-1]) if ps else None
    clean = re.sub(r"\x1b\[[0-9;]*m", "", text)
    ps_all = re.findall(r"\[PS\] (?:hold|release) .*? -> (\d+)", clean)
    result["ml_holds_end"] = int(ps_all[-1]) if ps_all else None
    # Card I/O is held per FIO call ("fio", src/fio-ml.c): ML reads its config, fonts and modules at
    # boot, so the pairs must show up, and every hold must be released.
    fio = re.findall(r"\[PS\] (hold|release) fio -> \d+", clean)
    result["fio_holds"] = fio.count("hold")
    result["fio_releases"] = fio.count("release")
    result["fio_holds_in_window"] = len(re.findall(r"\[PS\] hold fio -> ", window))
    result["ps_log"] = [l.strip()[-80:] for l in clean.splitlines() if "[PS] " in l and " fio -> " not in l][:40]
    result["pm_tail"] = [l.strip()[-100:] for l in upto.splitlines() if "[PM] " in l][-6:]
    # Who makes Canon toggle its counter during the window: the log lines (task switches,
    # other DebugMsgs) just before the first few DisablePowerSave messages.
    wl = window.splitlines()
    ctx = []
    for i, l in enumerate(wl):
        if "[PM] DisablePowerSave" in l:
            ctx.append([x.strip()[-100:] for x in wl[max(0, i - 8):i + 1]])
            if len(ctx) == 3:
                break
    result["pm_context"] = ctx
    result["self_refresh"] = len(re.findall(r"pmSelfRefresh", text))
    result["window_pm"] = pm_segment(window, a.measure, {})
    seg = result.get("display_off")
    if seg and "log" in seg:
        l0, l1, l2 = seg.pop("log")
        tasks = count_task_switches(log, l0, l1) if a.tasks else {}
        seg.update(pm_segment(read_log(log, l0, l1), seg["t1"] - seg["t0"], tasks))
        seg["tasks"] = tasks
        # A sleep ends after the wake key: its length is logged then, so count over both segments
        whole = pm_segment(read_log(log, l0, l2), 1, {})
        for k in ("qemu_self_refresh", "qemu_self_refresh_s", "qemu_self_refresh_max_s", "wake_irqs"):
            seg[k] = whole[k]
        seg["self_refresh_in_progress"] = whole["self_refresh_in_progress"]
        seg["plugin"] = plugin_window(intervals, seg["t0"] * 1000, seg["t1"] * 1000)
        wake = read_log(log, l1, l2)
        seg["wake"] = {"self_refresh_out": SR_RE.findall(wake).count("Out"),
                       "qemu_self_refresh_out": len(QSR_RE.findall(wake))}
        seg["idle_hash"] = frame_hash(result.get("idle_png"))
        seg["dark_hash"] = frame_hash(seg.get("dark_png"))
        seg["woken_hash"] = frame_hash(seg.get("woken_png"))
        seg["screen_changed_on_wake"] = bool(seg["dark_hash"] and seg["woken_hash"]
                                             and seg["dark_hash"] != seg["woken_hash"])
        seg["menu_hash"] = frame_hash(result.get("menu_png"))
        seg["menu_opened_after_wake"] = bool(seg["menu_hash"] and seg["menu_hash"] != seg["woken_hash"])
        # Card writes after the wake (config save when the menu closes): the fio holds must pair up
        after = re.findall(r"\[PS\] (hold|release) fio -> \d+", read_log(log, l1, os.path.getsize(log)))
        seg["fio_after_wake"] = {"holds": after.count("hold"), "releases": after.count("release")}

    # Power-save checks for ML scenarios whose DryosDebugMsg log is captured:
    #  - ML's holds are back to 0 at idle (end of the window) and after the menu closed;
    #  - the per-call FIO hold appears and is always released.
    #  - with --config-check, the config save after the deep sleep is checked by compare.py
    #    (file on the card) and its fio holds by the display-off segment above.
    failed = []
    infra = result.get("infra_error")
    if a.debugmsg_addr and not infra and not result.get("error") and result["pm_counter_window_end"] is None:
        failed.append("no Canon [PM] counter message in the log by the window end")
    # The counter address (--pm-counter-addr) must agree with Canon's own [PM] log: with no Disable/Enable in the
    # window, every RAM sample equals the last logged count (a wrong address shows up here).
    ctr = result.get("pm_counter_samples") or {}
    if (a.pm_counter_addr and a.debugmsg_addr and not infra and not result.get("error") and ctr
            and result["pm_counter_window_end"] is not None and result["pm_calls_in_window"] == 0
            and set(ctr) != {str(result["pm_counter_window_end"])}):
        failed.append("Canon counter at %s: samples %s, but the [PM] log says %d" % (
            a.pm_counter_addr, ",".join(sorted(ctr)), result["pm_counter_window_end"]))
    if a.boot and a.debugmsg_addr and not infra and not result.get("error"):
        if result["ml_holds_window_end"] is None:
            failed.append("no ML [PS] message in the log by the window end (ML not started?)")
        elif a.active_hold:
            if not (result["ml_holds_window_end"] or 0) >= 1:
                failed.append("ML holds at window end = %s, expected >= 1 (%s)" % (
                    result["ml_holds_window_end"], a.active_hold))
            if not any("hold %s " % a.active_hold in l for l in result["ps_log"]):
                failed.append("no [PS] hold %s in the log" % a.active_hold)
        elif result["ml_holds_window_end"] != 0:
            failed.append("ML holds at window end = %s, expected 0" % result["ml_holds_window_end"])
        if not a.active_hold and result["ml_holds_end"] != 0:
            failed.append("ML holds after the menu closed = %s, expected 0" % result["ml_holds_end"])
        if a.expect_modules:
            loaded = set(re.findall(r"\[MODULE\] loaded (\w+)", clean))
            result["modules_loaded"] = sorted(loaded)
            if "[MODULE] link failed" in clean:
                failed.append("module link failed (a module imports a symbol this build does not export)")
            missing = [m for m in a.expect_modules.split(",") if m and m not in loaded]
            if missing:
                failed.append("modules not loaded: %s" % ", ".join(missing))
        for t in result.get("menu_tour", []):
            if not t["changed"]:
                failed.append("menu tour: screen unchanged after key %s (%s)" % (
                    t["key"], os.path.basename(t["png"] or "?")))
        if result["fio_holds"] == 0:
            failed.append("no [PS] hold fio in the log")
        if result["fio_holds"] != result["fio_releases"]:
            failed.append("fio holds %d != releases %d" % (result["fio_holds"], result["fio_releases"]))
    result["checks_failed"] = failed
    with open(os.path.join(a.out, "result.json"), "w") as f:
        json.dump(result, f, indent=1)
    print(json.dumps(result))
    if infra:
        # Not a functional verdict either way: the emulator never got where the timeline needed it.
        print("INFRASTRUCTURE TIMEOUT (emulator too slow or unresponsive, no functional result): %s" % infra,
              file=sys.stderr)
        sys.exit(EXIT_INFRA)
    if failed:
        print("power-save checks FAILED: %s" % "; ".join(failed), file=sys.stderr)
        sys.exit(1)
    if result.get("error") or not summary or not summary.get("seconds"):
        print("no plugin summary for the measurement window", file=sys.stderr)
        sys.exit(1)
    if not win_stats or win_stats["seconds"] < 0.9 * a.measure:
        print("plugin interval records cover %s s of the %.0f s window" % (
            win_stats and win_stats["seconds"], a.measure), file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
