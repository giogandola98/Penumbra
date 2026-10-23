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
    t0 = time.monotonic()
    with open(log, "wb") as logf:
        q = subprocess.Popen(cmd, env=env, stdin=subprocess.DEVNULL, stdout=logf, stderr=subprocess.STDOUT)
    result = {"cam": a.cam, "boot_ml": a.boot, "warmup_s": a.warmup, "measure_s": a.measure}
    log_start = log_end = 0
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
        pcs = {}
        end = t0 + a.warmup + a.measure
        while time.monotonic() < end - 0.5:
            m = re.search(r"R15=([0-9a-f]{8})", monitor_cmd(mon, "info registers", timeout=2))
            if m:
                pcs[m.group(1)] = pcs.get(m.group(1), 0) + 1
            time.sleep(min(0.5, max(0, end - time.monotonic())))
        time.sleep(max(0, end - time.monotonic()))
        n = sum(pcs.values())
        result["pc_samples"] = n
        result["top_pcs"] = [("0x" + pc, round(c / n, 3)) for pc, c in
                             sorted(pcs.items(), key=lambda kv: -kv[1])[:10]] if n else []
        log_end = os.path.getsize(log)
        result["idle_png"] = screenshot(mon, os.path.join(a.out, "idle.png"))
        if a.boot:
            # After the window, so it cannot skew the metrics: open the ML menu (Trash = Delete key
            # in qemu-eos) and capture it.
            monitor_cmd(mon, "sendkey delete")
            time.sleep(3)
            result["menu_png"] = screenshot(mon, os.path.join(a.out, "menu.png"))
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
    if os.path.exists(wj):
        for line in open(wj):
            rec = json.loads(line)
            if rec.get("type") == "summary":
                summary = rec
    result["wakeups"] = summary
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
    with open(os.path.join(a.out, "result.json"), "w") as f:
        json.dump(result, f, indent=1)
    print(json.dumps(result))
    if result.get("error") or not summary or not summary.get("seconds"):
        print("no plugin summary for the measurement window", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
