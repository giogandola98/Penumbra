#!/usr/bin/env python3
"""Boot one Canon camera scenario in qemu-eos headless and measure it.

Timeline (host time, which QEMU's virtual clock follows without -icount):
  0 .. warmup        boot (Canon firmware, then ML if --boot); not measured
  warmup .. +measure measurement window: wakeups plugin summary, task switches from `-d tasks`
  end                screenshot via the monitor, then `quit` (the plugin writes its summary at exit)

Then, optionally (--display-off-s, 1100D): the display-off idle segment (deep sleep, see
docs/analysis/powersave-hold.md), a key to wake the camera, and for ML builds the ML menu and a
config save.

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


def monitor_cmd(path, cmd, timeout=10.0):
    """Send one HMP command over the monitor socket and return what QEMU printed back."""
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.connect(path)
    buf = b""
    try:
        buf += s.recv(4096)                       # banner + prompt
        s.sendall(cmd.encode() + b"\n")
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline and buf.count(b"(qemu)") < 2:
            chunk = s.recv(4096)
            if not chunk:
                break
            buf += chunk
    except socket.timeout:
        pass
    finally:
        s.close()
    return buf.decode(errors="replace")


def screenshot(mon, out_png):
    ppm = out_png[:-4] + ".ppm"
    try:
        monitor_cmd(mon, "screendump " + ppm)
    except OSError as e:
        print("screenshot failed: %s" % e, file=sys.stderr)
        return None
    for _ in range(50):                           # screendump completes asynchronously
        if os.path.exists(ppm) and os.path.getsize(ppm) > 0:
            break
        time.sleep(0.1)
    try:
        from PIL import Image
        time.sleep(0.2)
        with Image.open(ppm) as im:
            im.save(out_png)
        os.remove(ppm)
        return out_png
    except Exception as e:                        # a missing screenshot must not lose the metrics
        print("screenshot failed: %s" % e, file=sys.stderr)
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


def sample(mon, end, pm_addr):
    """Until monotonic time `end`: guest PC and Canon's power-save counter every ~0.5 s via the monitor."""
    pcs, ctr = {}, {}
    while time.monotonic() < end - 0.5:
        m = re.search(r"R15=([0-9a-f]{8})", monitor_cmd(mon, "info registers", timeout=2))
        if m:
            pcs[m.group(1)] = pcs.get(m.group(1), 0) + 1
        if pm_addr:
            m = re.search(r":\s*0x([0-9a-f]+)", monitor_cmd(mon, "xp /1wx " + pm_addr, timeout=2))
            if m:
                v = str(int(m.group(1), 16))
                ctr[v] = ctr.get(v, 0) + 1
        time.sleep(min(0.5, max(0, end - time.monotonic())))
    time.sleep(max(0, end - time.monotonic()))
    return pcs, ctr


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
    t_quit = None
    try:
        for _ in range(600):
            if os.path.exists(mon) or q.poll() is not None:
                break
            time.sleep(0.1)
        if q.poll() is not None:
            raise RuntimeError("QEMU exited early with code %s (see qemu.log)" % q.returncode)
        if not os.path.exists(mon):
            raise RuntimeError("QEMU monitor did not appear in 60 s")
        # The timeline starts when QEMU is up, not at Popen: QEMU loads its plugins (whose window is
        # timed from their load) just before it creates the monitor socket, and a cold start of the
        # binary on a busy runner took ~10 s, which used to cut the measurement window by as much.
        result["qemu_start_s"] = round(time.monotonic() - t0, 2)
        t0 = time.monotonic()

        time.sleep(max(0, a.warmup - (time.monotonic() - t0)))
        log_start = os.path.getsize(log)
        result["boot_png"] = screenshot(mon, os.path.join(a.out, "boot.png"))
        # Sample the guest PC through the monitor during the window: a ROM-independent profile that
        # shows where the CPU spends its time (e.g. an idle loop that spins instead of sleeping).
        # Canon's power-save counter is sampled the same way: a cross-check of the exact count of its
        # changes, which comes from the [PM] messages Canon logs on every Disable/EnablePowerSave.
        pcs, ctr = sample(mon, t0 + a.warmup + a.measure, a.pm_counter_addr)
        result["pc_samples"] = sum(pcs.values())
        result["top_pcs"] = top(pcs)
        if a.pm_counter_addr:
            result["pm_counter_samples"] = ctr
        log_end = os.path.getsize(log)
        result["idle_png"] = screenshot(mon, os.path.join(a.out, "idle.png"))
        if a.display_off_s > 0:
            # Display off and idle: Canon's PowerMgr may enter selfRefresh (all clocks off, DryOS
            # timers stopped) once the image engine clock is off and nothing restamped it for ~1 s.
            # Then the same key wakes the camera; the screen must change (GUI alive after the sleep).
            seg = {"key": a.display_key, "seconds": a.display_off_s}
            seg_log0 = os.path.getsize(log)
            seg["t0"] = round(time.monotonic() - t0, 2)
            monitor_cmd(mon, "sendkey " + a.display_key)
            dpcs, dctr = sample(mon, time.monotonic() + a.display_off_s, a.pm_counter_addr)
            seg["t1"] = round(time.monotonic() - t0, 2)
            seg_log1 = os.path.getsize(log)
            seg["top_pcs"] = top(dpcs, 5)
            seg["pm_counter_samples"] = dctr
            seg["dark_png"] = screenshot(mon, os.path.join(a.out, "display_off.png"))
            monitor_cmd(mon, "sendkey " + a.display_key)
            time.sleep(4)
            seg["woken_png"] = screenshot(mon, os.path.join(a.out, "woken.png"))
            seg_log2 = os.path.getsize(log)
            seg["log"] = [seg_log0, seg_log1, seg_log2]
            result["display_off"] = seg
        if a.boot and not a.no_menu:
            # After the window, so it cannot skew the metrics: open the ML menu (Trash = Delete key
            # in qemu-eos) and capture it.
            # The menu is a consumer of lens info outside LiveView: Canon's counter changes counted
            # while it is open show whether ML still refreshes it (docs/analysis/powersave-hold.md).
            menu_start = os.path.getsize(log)
            monitor_cmd(mon, "sendkey delete")
            time.sleep(max(3, a.menu_s))
            menu_end = os.path.getsize(log)
            result["menu_png"] = screenshot(mon, os.path.join(a.out, "menu.png"))
            # Close it again: ML's idle state must come back with no hold left (checked below).
            # On a fresh card the ML menu opens on its beta warning, which the first MENU only dismisses.
            # --config-check: then SET changes the selected entry (config dirty) and MENU closes the
            # menu, so ML autosaves its config: a file write after the deep sleep (checked on the card
            # by run_cam.sh, fio holds checked below). Otherwise MENU twice: the second closes the menu
            # (or, without the warning, opens Canon's menu, which ML does not hold). Trash would reopen it.
            keys = ["m", "spc", "m"] if a.config_check else ["m", "m"]
            for k in keys:
                monitor_cmd(mon, "sendkey " + k)
                time.sleep(2)
                if k == "spc":
                    result["menu_set_png"] = screenshot(mon, os.path.join(a.out, "menu_set.png"))
            time.sleep(3)
            result["menu_closed_png"] = screenshot(mon, os.path.join(a.out, "menu_closed.png"))
        if q.poll() is not None:
            raise RuntimeError("QEMU died during the run, code %s (see qemu.log)" % q.returncode)
        t_quit = time.monotonic() - t0
        monitor_cmd(mon, "quit")
        q.wait(timeout=30)
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
    win_stats = plugin_window(intervals, a.warmup * 1000, (a.warmup + a.measure) * 1000) if summary else None
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
        result["menu_s"] = max(3, a.menu_s)
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
    if a.boot and a.debugmsg_addr and not result.get("error"):
        if a.active_hold:
            if not (result["ml_holds_window_end"] or 0) >= 1:
                failed.append("ML holds at window end = %s, expected >= 1 (%s)" % (
                    result["ml_holds_window_end"], a.active_hold))
            if not any("hold %s " % a.active_hold in l for l in result["ps_log"]):
                failed.append("no [PS] hold %s in the log" % a.active_hold)
        else:
            if result["ml_holds_window_end"] != 0:
                failed.append("ML holds at window end = %s, expected 0" % result["ml_holds_window_end"])
            if result["ml_holds_end"] != 0:
                failed.append("ML holds after the menu closed = %s, expected 0" % result["ml_holds_end"])
        if result["fio_holds"] == 0:
            failed.append("no [PS] hold fio in the log")
        if result["fio_holds"] != result["fio_releases"]:
            failed.append("fio holds %d != releases %d" % (result["fio_holds"], result["fio_releases"]))
    result["checks_failed"] = failed
    with open(os.path.join(a.out, "result.json"), "w") as f:
        json.dump(result, f, indent=1)
    print(json.dumps(result))
    if failed:
        print("power-save checks FAILED: %s" % "; ".join(failed), file=sys.stderr)
        sys.exit(1)
    if result.get("error") or not summary or not summary.get("seconds"):
        print("no plugin summary for the measurement window", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
