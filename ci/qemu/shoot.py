#!/usr/bin/env python3
"""Shooting scenarios: take pictures in qemu-eos and check what landed on the card.

Needs a QEMU-EOS-Optimized image with photo capture emulation (hw/eos/capture.c, docs/models/capture.md
there): a full shutter press or a remote release runs Canon's shooting sequence and the picture is written
to the emulated SD card with Canon's EXIF. The emulator logs each release as
"[CAPTURE] shot n: tv=0x.. av=0x.. iso=0x.. ae=0x.. mode=n" (the exposure the DIGIC set, i.e. what ML
asked for), Canon logs "iso:0x..,tv:0x..,av:0x.." when it starts the capture, and the EXIF of the file
holds exposure time, aperture, ISO and exposure bias.

Scenarios (generic; which ones run and the camera specifics are in CAMERAS below, one entry per camera):
  canon-shot   Canon firmware only: full shutter press (left Ctrl) -> one picture
  ml-ivl       ML intervalometer from a config preset: 3 shots, 5 s apart, started at boot
  ml-hdr       ML HDR bracketing (3 frames, 2 EV) from 1/100 s: one press, ML
               takes the other two frames; the exposures must be base, base -2 EV and base +2 EV (Tv, M mode)
  ml-bulb      ML bulb exposure (bulb_take_pic, driven by the intervalometer) in BULB, one 3 s shot

"tv": once the guest is ready the emulated MPU reports that shutter speed, as if the main dial had been turned
(PROP_SHUTTER through qemu-eos' spell injection: QEMU_EOS_SPELLS_DIR + F5), so Canon, ML and the MPU agree on it.

Timeline: QEMU starts -> monitor socket -> the guest is ready (CAMERAS ready markers in the DryosDebugMsg
log) -> trigger -> wait for the pictures ("write done" lines) -> quit -> the card is read (card.py).
A step the emulator never reached because it did not come up (no monitor, no ready marker) is an
INFRASTRUCTURE TIMEOUT (exit 3); pictures that do not come once the guest is running are a failure (exit 1).

Usage: shoot.py --cam CAM --scenario NAME --rom-dir DIR --base-sd sd.qcow2.xz [--zip ML.zip]
                --debugmsg-addr ADDR --out DIR
       shoot.py --list CAM [--ml]        scenarios for a camera (with --ml: also the ML ones)
"""
import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)          # python3 -I drops the script dir
import card                       # noqa: E402
from scenario import InfraTimeout, monitor_cmd, screenshot, EXIT_INFRA   # noqa: E402

QEMU_EOS = os.environ.get("QEMU_EOS", "/opt/qemu-eos")

# Per camera, in one place: readiness markers (DryosDebugMsg), the key of a full shutter press, and the
# scenarios that run on it. ML scenarios need the build passed as --zip.
CAMERAS = {
    "1100D": {
        "canon_ready": ["TurnOnDisplay finish"],
        "ml_ready": [r"\[PS\] release boot ->"],
        "shutter_key": "ctrl",
        "scenarios": ["canon-shot", "ml-ivl", "ml-hdr", "ml-bulb"],
    },
    "4000D": {
        "canon_ready": ["startupInitializeComplete"],
        "ml_ready": [r"\[PS\] release boot ->"],
        "shutter_key": "ctrl",
        "scenarios": ["canon-shot", "ml-ivl", "ml-hdr", "ml-bulb"],
    },
}

# Generic scenario definitions: ML config preset, trigger, pictures expected, emulator environment.
SCENARIOS = {
    "canon-shot": {"ml": False, "press": True, "pictures": 1},
    "ml-ivl": {"ml": True, "press": False, "pictures": 3,
               "preset": ["interval.enabled = 1", "interval.time = 5", "interval.start.time = 3",
                          "interval.stop.after = 3"]},
    "ml-hdr": {"ml": True, "press": True, "pictures": 3,
               # base 1/100 s: all frames stay short (1/400 .. 1/25 s); exposures over ~1 s take a
               # long-exposure correction path the 4000D model does not always finish
               "tv": 0x6D,
               "preset": ["hdr.enabled = 1", "hdr.frames = 3", "hdr.ev_spacing = 16"]},
    "ml-bulb": {"ml": True, "press": False, "pictures": 1,
                "tv": 0x0C,
                "preset": ["interval.enabled = 1", "interval.time = 10", "interval.start.time = 3",
                           "interval.stop.after = 1", "bulb.duration = 3"]},
}

SHOT_RE = re.compile(r"\[CAPTURE\] shot (\d+): tv=0x([0-9A-F]+) av=0x([0-9A-F]+) iso=0x([0-9A-F]+) "
                     r"ae=0x([0-9A-F]+) mode=(\d+)")
CANON_RE = re.compile(r"iso:0x([0-9a-f]+),tv:0x([0-9a-f]+),av:0x([0-9a-f]+)")


def make_cards(a, sc, work):
    """SD card (base image, ML build, ROM-backup seeds, config preset) and an empty CF card."""
    sd, cf = os.path.join(work, "sd.img"), os.path.join(work, "cf.img")
    args = []
    if sc["ml"]:
        for r in ("ROM0", "ROM1"):       # ML would back the ROMs up on first boot (see run_cam.sh)
            p = os.path.join(a.rom_dir, a.cam, r + ".BIN")
            if os.path.exists(p):
                args += ["--seed", "ML/LOGS/%s.BIN:%d" % (r, os.path.getsize(p))]
        if sc.get("preset"):
            cfg = os.path.join(work, "magic.cfg")
            with open(cfg, "w", newline="") as f:
                f.write("".join(l + "\r\n" for l in sc["preset"]))
            args += ["--put", "ML/SETTINGS/MAGIC.CFG:" + cfg]
    mk = [sys.executable, "-I", os.path.join(HERE, "make_sd_image.py")]
    subprocess.run(mk + args + [a.base_sd, sd] + ([a.zip] if sc["ml"] else []), check=True,
                   stdout=subprocess.DEVNULL)
    subprocess.run(mk + [a.base_sd, cf], check=True, stdout=subprocess.DEVNULL)
    return sd, cf


def wait_log_count(log, pattern, n, timeout, q):
    """Seconds until `pattern` (regex) matched n times in the log; None on timeout."""
    t = time.monotonic()
    while True:
        with open(log, "rb") as f:
            text = re.sub(r"\x1b\[[0-9;]*m", "", f.read().decode(errors="replace"))
        if len(re.findall(pattern, text)) >= n:
            return time.monotonic() - t
        if q.poll() is not None:
            raise RuntimeError("QEMU exited with code %s (see qemu.log)" % q.returncode)
        if time.monotonic() - t > timeout:
            return None
        time.sleep(0.5)


def check(name, sc, text, pictures, cam_shots):
    """Functional checks; returns a list of failures."""
    errors = []
    n = sc["pictures"]
    if len(cam_shots) < n:
        errors.append("%d releases (emulator '[CAPTURE] shot' lines), expected %d" % (len(cam_shots), n))
    if len(pictures) < n:
        errors.append("%d pictures on the card, expected %d" % (len(pictures), n))
    for p in pictures:
        if not p["exif"].get("exposure_time"):
            errors.append("%s: no EXIF exposure time" % p["path"])
    canon = CANON_RE.findall(text)
    if len(canon) < n:
        errors.append("%d Canon capture starts ('iso:..,tv:..,av:..'), expected %d" % (len(canon), n))
    # what Canon captured is what the MPU reported for that release
    for s, c in zip(cam_shots, canon):
        iso_ok = int(c[0], 16) == s["iso"] or (s["iso"] == 0 and int(c[0], 16) > 0)   # 0 = auto ISO
        if int(c[1], 16) != s["tv"] or int(c[2], 16) != s["av"] or not iso_ok:
            errors.append("shot %d: Canon captured tv/av/iso 0x%s/0x%s/0x%s, MPU reported 0x%02X/0x%02X/0x%02X"
                          % (s["n"], c[1], c[2], c[0], s["tv"], s["av"], s["iso"]))
    if name == "ml-ivl" and text.count("[CAPTURE] remote SW2 on") < n:
        errors.append("ML's releases did not go through the remote release (PROP_REMOTE_SW2)")
    if name == "ml-hdr" and len(cam_shots) >= 3:
        base = cam_shots[0]["tv"]
        got = sorted(s["tv"] - base for s in cam_shots[:3])
        if got != [-16, 0, 16]:
            errors.append("HDR Tv steps %s (raw, vs the first frame), expected [-16, 0, 16] (+-2 EV)" % got)
        times = [p["exif"].get("exposure_time") for p in pictures[:3]]
        if len(set(times)) != 3:
            errors.append("HDR: EXIF exposure times not all different: %s" % times)
    if name == "ml-bulb":
        if not cam_shots or cam_shots[0]["tv"] != 0x0C:
            errors.append("bulb: the release was not in BULB (tv 0x0C)")
        if "[CAPTURE] bulb end" not in text:
            errors.append("bulb: no bulb end (EVENTID_BULB_END)")
    return errors


def main():
    ap = argparse.ArgumentParser(usage=__doc__)
    ap.add_argument("--list", metavar="CAM")
    ap.add_argument("--ml", action="store_true", help="--list: include the ML scenarios")
    ap.add_argument("--cam")
    ap.add_argument("--scenario")
    ap.add_argument("--rom-dir")
    ap.add_argument("--base-sd")
    ap.add_argument("--zip", default="")
    ap.add_argument("--debugmsg-addr", default="")
    ap.add_argument("--out")
    ap.add_argument("--start-timeout", type=float, default=180)
    ap.add_argument("--ready-timeout", type=float, default=300)
    ap.add_argument("--picture-timeout", type=float, default=float(os.environ.get("SHOOT_TIMEOUT_S", 240)),
                    help="for all the pictures, once the guest is ready (the intervalometer's first shot "
                         "comes 15 s after ML's start)")
    a = ap.parse_args()

    if a.list:
        cam = CAMERAS.get(a.list, {"scenarios": []})
        print(" ".join(s for s in cam["scenarios"] if a.ml or not SCENARIOS[s]["ml"]))
        return
    cam, sc = CAMERAS[a.cam], SCENARIOS[a.scenario]
    if sc["ml"] and not a.zip:
        sys.exit("%s needs --zip" % a.scenario)
    os.makedirs(a.out, exist_ok=True)
    work = tempfile.mkdtemp(prefix="shoot-")      # disk images: never kept, never uploaded
    log = os.path.join(a.out, "qemu.log")
    mon = os.path.join(work, "qemu.monitor")
    result = {"cam": a.cam, "scenario": a.scenario, "errors": []}
    status = 0
    q = None
    try:
        sd, cf = make_cards(a, sc, work)
        cmd = [os.path.join(QEMU_EOS, "bin", "qemu-system-arm"),
               "-M", "%s,firmware=boot=%d" % (a.cam, 1 if sc["ml"] else 0),
               "-drive", "if=sd,format=raw,file=" + sd, "-drive", "if=ide,format=raw,file=" + cf,
               "-chardev", "socket,server,nowait,path=%s,id=monsock" % mon,
               "-mon", "chardev=monsock,mode=readline",
               "-display", "none", "-serial", "file:" + os.path.join(a.out, "uart.log"), "-name", a.cam]
        if a.debugmsg_addr:
            cmd += ["-plugin", os.path.join(QEMU_EOS, "plugins", "libmagiclantern.so")
                    + ",arg=debugmsg_addr=" + a.debugmsg_addr, "-d", "debugmsg"]
        env = dict(os.environ, QEMU_EOS_WORKDIR=a.rom_dir)
        if "tv" in sc:
            with open(os.path.join(work, "f5.txt"), "w") as f:
                f.write("06 05 01 05 %02x 00\n" % sc["tv"])      # PROP_SHUTTER from the MPU
            env["QEMU_EOS_SPELLS_DIR"] = work
        print(" ".join(cmd), flush=True)
        t0 = time.monotonic()
        with open(log, "wb") as logf:
            q = subprocess.Popen(cmd, env=env, stdin=subprocess.DEVNULL, stdout=logf, stderr=subprocess.STDOUT)
        while not os.path.exists(mon) and q.poll() is None and time.monotonic() - t0 < a.start_timeout:
            time.sleep(0.1)
        if q.poll() is not None:
            raise RuntimeError("QEMU exited early with code %s (see qemu.log)" % q.returncode)
        if not os.path.exists(mon):
            raise InfraTimeout("QEMU monitor did not appear within %.0f s" % a.start_timeout)
        t0 = time.monotonic()
        markers = cam["ml_ready"] if sc["ml"] else cam["canon_ready"]
        for m in markers:
            if wait_log_count(log, m, 1, a.ready_timeout - (time.monotonic() - t0), q) is None:
                raise InfraTimeout("guest not ready %.0f s after QEMU start: no '%s'" % (a.ready_timeout, m))
        result["ready_s"] = round(time.monotonic() - t0, 1)
        time.sleep(3)                      # let the shooting screen settle
        if "tv" in sc:
            monitor_cmd(mon, "sendkey f5")
            if wait_log_count(log, r"\[CAPTURE\] injected", 1, 30, q) is None:
                raise RuntimeError("the emulator did not take the shutter speed (needs capture emulation)")
            time.sleep(2)
        screenshot(mon, os.path.join(a.out, "ready.png"))
        if sc["press"]:
            monitor_cmd(mon, "sendkey " + cam["shutter_key"])
        tp = time.monotonic()
        waited = wait_log_count(log, r"write done", sc["pictures"], a.picture_timeout, q)
        result["pictures_s"] = None if waited is None else round(time.monotonic() - tp, 1)
        if waited is None:
            with open(log, "rb") as f:
                done = f.read().count(b"write done")
            result["errors"].append("only %d of %d pictures written after %.0f s" % (
                done, sc["pictures"], a.picture_timeout))
        time.sleep(3)                      # Canon's catalog update after the last file
        screenshot(mon, os.path.join(a.out, "after.png"))
        try:
            monitor_cmd(mon, "quit")
        except InfraTimeout:
            pass
        q.wait(timeout=30)
    except InfraTimeout as e:
        result["errors"].append("INFRASTRUCTURE TIMEOUT: %s" % e)
        status = EXIT_INFRA
    except (RuntimeError, OSError, subprocess.SubprocessError) as e:
        result["errors"].append(str(e))
        status = 1
    finally:
        if q is not None and q.poll() is None:
            q.kill()
            q.wait()

    text = re.sub(r"\x1b\[[0-9;]*m", "", open(log, "rb").read().decode(errors="replace")) \
        if os.path.exists(log) else ""
    shots = [{"n": int(m[0]), "tv": int(m[1], 16), "av": int(m[2], 16), "iso": int(m[3], 16),
              "ae": int(m[4], 16), "mode": int(m[5])} for m in SHOT_RE.findall(text)]
    pictures = []
    sd = os.path.join(work, "sd.img")
    if os.path.exists(sd):
        try:
            pictures = card.pictures(sd)
        except Exception as e:
            result["errors"].append("card: %s" % e)
    result["shots"] = shots
    result["pictures"] = pictures
    if status != EXIT_INFRA:
        result["errors"] += check(a.scenario, sc, text, pictures, shots)
    for f in ("sd.img", "cf.img", "magic.cfg", "f5.txt"):
        try:
            os.remove(os.path.join(work, f))
        except FileNotFoundError:
            pass
    try:
        os.rmdir(work)
    except OSError:
        pass

    with open(os.path.join(a.out, "result.json"), "w") as f:
        json.dump(result, f, indent=1)
    for s, p in zip(shots, pictures + [None] * len(shots)):
        e = p["exif"] if p else {}
        print("  shot %d: tv=0x%02X av=0x%02X iso=0x%02X ae=0x%02X -> %s %s s f/%s ISO %s" % (
            s["n"], s["tv"], s["av"], s["iso"], s["ae"], p["path"] if p else "(no file)",
            e.get("exposure_time"), e.get("f_number"), e.get("iso")))
    if result["errors"]:
        print("%s/%s: %s" % (a.cam, a.scenario, "; ".join(result["errors"])))
        status = status or 1
    else:
        print("%s/%s: OK, %d pictures" % (a.cam, a.scenario, len(pictures)))
    sys.exit(status)


if __name__ == "__main__":
    main()
