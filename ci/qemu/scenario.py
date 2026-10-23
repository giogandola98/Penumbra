#!/usr/bin/env python3
"""Boot one Canon camera scenario in qemu-eos headless and measure it.

Timeline (host time, which QEMU's virtual clock follows without -icount):
  0 .. warmup        boot (Canon firmware, then ML if --boot); not measured
  warmup .. +measure measurement window: wakeups plugin summary, task switches from `-d tasks`
  end                screenshot via the monitor, then `quit` (the plugin writes its summary at exit)

Writes into OUT_DIR: qemu.log (DryosDebugMsg + task switches + QEMU messages), wakeups.json,
tasks.json (task switches per task inside the window), boot.png / idle.png, result.json.
"""
import argparse
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
        deadline = time.time() + timeout
        while time.time() < deadline and buf.count(b"(qemu)") < 2:
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
    ap.add_argument("--warmup", type=float, default=40)
    ap.add_argument("--measure", type=float, default=30)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    os.makedirs(a.out, exist_ok=True)
    mon = os.path.join(a.out, "qemu.monitor")
    log = os.path.join(a.out, "qemu.log")
    wj = os.path.join(a.out, "wakeups.json")

    plugin = "%s,arg=out=%s,arg=start_ms=%d,arg=interval_ms=%d" % (
        a.plugin, wj, int(a.warmup * 1000), 10000)
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
    t0 = time.time()
    with open(log, "wb") as logf:
        q = subprocess.Popen(cmd, env=env, stdin=subprocess.DEVNULL, stdout=logf, stderr=subprocess.STDOUT)
    result = {"cam": a.cam, "boot_ml": a.boot, "warmup_s": a.warmup, "measure_s": a.measure}
    log_start = log_end = 0
    menu_start = menu_end = 0
    try:
        for _ in range(100):
            if os.path.exists(mon) or q.poll() is not None:
                break
            time.sleep(0.1)
        if q.poll() is not None:
            raise RuntimeError("QEMU exited early with code %s (see qemu.log)" % q.returncode)

        time.sleep(max(0, a.warmup - (time.time() - t0)))
        log_start = os.path.getsize(log)
        result["boot_png"] = screenshot(mon, os.path.join(a.out, "boot.png"))
        # Sample the guest PC through the monitor during the window: a ROM-independent profile that
        # shows where the CPU spends its time (e.g. an idle loop that spins instead of sleeping).
        # Canon's power-save counter is sampled the same way: a cross-check of the exact count of its
        # changes, which comes from the [PM] messages Canon logs on every Disable/EnablePowerSave.
        pcs = {}
        ctr = {}
        end = t0 + a.warmup + a.measure
        while time.time() < end - 0.5:
            m = re.search(r"R15=([0-9a-f]{8})", monitor_cmd(mon, "info registers", timeout=2))
            if m:
                pcs[m.group(1)] = pcs.get(m.group(1), 0) + 1
            if a.pm_counter_addr:
                m = re.search(r":\s*0x([0-9a-f]+)", monitor_cmd(mon, "xp /1wx " + a.pm_counter_addr, timeout=2))
                if m:
                    v = str(int(m.group(1), 16))
                    ctr[v] = ctr.get(v, 0) + 1
            time.sleep(min(0.5, max(0, end - time.time())))
        time.sleep(max(0, end - time.time()))
        n = sum(pcs.values())
        result["pc_samples"] = n
        result["top_pcs"] = [("0x" + pc, round(c / n, 3)) for pc, c in
                             sorted(pcs.items(), key=lambda kv: -kv[1])[:10]] if n else []
        if a.pm_counter_addr:
            result["pm_counter_samples"] = ctr
        log_end = os.path.getsize(log)
        result["idle_png"] = screenshot(mon, os.path.join(a.out, "idle.png"))
        if a.boot:
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
            # MENU twice: on a fresh card the first press only dismisses ML's beta warning and
            # the second closes the menu; without the warning the first closes it and the second
            # opens Canon's menu, which ML does not hold. (Trash would reopen the ML menu.)
            for _ in range(2):
                monitor_cmd(mon, "sendkey m")
                time.sleep(2)
            time.sleep(2)
        if q.poll() is not None:
            raise RuntimeError("QEMU died during the run, code %s (see qemu.log)" % q.returncode)
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
    if os.path.exists(wj):
        for line in open(wj):
            rec = json.loads(line)
            if rec.get("type") == "summary":
                summary = rec
    result["wakeups"] = summary
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
    # Power-save checks for ML scenarios whose DryosDebugMsg log is captured:
    #  - ML's holds are back to 0 at idle (end of the window) and after the menu closed;
    #  - the per-call FIO hold appears and is always released.
    # Pending: a config save from the menu (needs a menu change to make the config dirty).
    failed = []
    if a.boot and a.debugmsg_addr and not result.get("error"):
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
