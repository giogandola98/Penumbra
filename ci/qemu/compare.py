#!/usr/bin/env python3
"""Markdown comparison table of the scenario results written by scenario.py.

Usage: compare.py CAM OUT_DIR SCENARIO...
Deltas are relative to the first scenario (the Canon-only baseline).
With display-off results (scenario.py --display-off-s) it also prints the deep-sleep table and checks
the expectations of docs/analysis/powersave-hold.md; DEEP_SLEEP_ASSERT=1 turns a failed check into exit 1.
"""
import json
import os
import re
import sys


def load(out, sc):
    try:
        with open(os.path.join(out, sc, "result.json")) as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def fmt(v, nd=1):
    return "n/a" if v is None else ("%.*f" % (nd, v))


def settings_saved(out, sc):
    """ML wrote its config during the run and the file is intact: ML/SETTINGS/MAGIC.CFG on the card
    after the run (run_cam.sh) has ML's header, only `name = integer` lines, and beta.warn, which ML
    sets when its warning is dismissed in this run (absent from any seeded preset)."""
    try:
        with open(os.path.join(out, sc, "sd_settings.txt"), errors="replace") as f:
            text = f.read()
    except OSError:
        return None
    cfg = text.split("--- MAGIC.CFG", 1)[-1].strip().splitlines()
    body = [l.strip() for l in cfg if l.strip() and not l.startswith("#")]
    return ("# Configuration saved on" in text and bool(body)
            and all(re.match(r"^[\w.]+ = -?\d+$", l) for l in body)
            and any(l.startswith("beta.warn = ") for l in body))


# Expected deep-sleep behaviour per scenario: does Canon enter selfRefresh with the display off?
EXPECT_SLEEP = {"canon": True, "ml-full": False, "ml-photo": None, "ml-hold": True, "ml-hold-ivl": False}


def deep_sleep(out, rows):
    """Table + checks for the display-off segment. Returns the list of failed checks."""
    rows = [(sc, r) for sc, r in rows if r and r.get("display_off")]
    if not rows:
        return []
    print("\n## Deep sleep: display off (%s key) for %.0f s after the window, then the same key\n" % (
        rows[0][1]["display_off"]["key"], rows[0][1]["display_off"]["seconds"]))
    print("| scenario | pmSelfRefresh In (display off) / Out (after key) | sleeps (emulator) | in self-refresh s "
          "| wake irq | halted s | busy | WFI/s | IRQ/s | task sw/s | Canon counter (samples) | ML holds "
          "| woken: screen changed | ML menu opened | config saved |")
    print("|---|---:|---:|---:|---|---:|---:|---:|---:|---:|---|---|---|---|---|")
    fails = []
    for sc, r in rows:
        d = r["display_off"]
        p = d.get("plugin") or {}
        ctr = d.get("pm_counter_samples") or {}
        saved = settings_saved(out, sc) if r.get("boot_ml") else None
        holds = "%s%s" % (r.get("ml_holds_window_end", "-"), (" (%s)" % "; ".join(
            l.split("[PS] ", 1)[-1] for l in r.get("ps_log", []) if "hold" in l and "boot" not in l))
            if sc == "ml-hold-ivl" else "")
        print("| %s | %d/%d | %d%s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |" % (
            sc, d["self_refresh_in"], (d.get("wake") or {}).get("self_refresh_out", 0), d["qemu_self_refresh"],
            " (+1 at end)" if d.get("self_refresh_in_progress") else "",
            fmt(d["qemu_self_refresh_s"], 1), ",".join(d.get("wake_irqs") or []) or "-", fmt(p.get("halted_s"), 1),
            fmt(p.get("busy_ratio"), 3), fmt(p.get("wakeups_per_s")), fmt(p.get("irq_per_s")),
            fmt(d.get("task_switches_per_s")), ", ".join("%s=%d" % kv for kv in sorted(ctr.items())) or "n/a",
            holds if r.get("boot_ml") else "-",
            "yes" if d.get("screen_changed_on_wake") else "NO",
            ("yes" if d.get("menu_opened_after_wake") else "NO") if r.get("menu_png") else "-",
            {True: "yes", False: "NO", None: "-"}[saved]))
        want = EXPECT_SLEEP.get(sc)
        slept = d["self_refresh_in"] > 0
        checks = []
        if want is not None:
            checks.append(("deep sleep %s" % ("entered" if want else "never entered"), slept == want))
        if want is False and ctr:
            checks.append(("Canon counter >= 1 in every sample", all(int(v) >= 1 for v in ctr)))
        if want:
            checks.append(("emulator: clocks gated, then woken by the key",
                           d["qemu_self_refresh"] > 0 and (d.get("wake") or {}).get("self_refresh_out", 0) > 0))
        checks.append(("screen changes after the wake key", bool(d.get("screen_changed_on_wake"))))
        if r.get("menu_png"):
            checks.append(("ML menu opens after the wake", bool(d.get("menu_opened_after_wake"))))
        if r.get("menu_closed_png") and r.get("menu_set_png"):
            checks.append(("ML config saved after the wake, file intact", bool(saved)))
            fw = d.get("fio_after_wake") or {}
            checks.append(("card writes after the wake: fio holds pair (%d/%d)" % (
                fw.get("holds", 0), fw.get("releases", 0)),
                fw.get("holds", 0) > 0 and fw.get("holds") == fw.get("releases")))
        if sc == "ml-hold-ivl":
            checks.append(("intervalometer hold taken", any("intervalometer" in l for l in r.get("ps_log", []))))
            tasks = d.get("tasks") or {}
            checks.append(("ML tasks keep switching with the display off",
                           (d.get("task_switches_per_s") or 0) >= 10 and tasks.get("shoot_task", 0) > 0))
        for name, ok in checks:
            print("%s %s: %s" % ("PASS" if ok else "FAIL", sc, name))
            if not ok:
                fails.append("%s: %s" % (sc, name))
    return fails


def main():
    cam, out, scenarios = sys.argv[1], sys.argv[2], sys.argv[3:]
    rows = [(sc, load(out, sc)) for sc in scenarios]
    base = rows[0][1]["wakeups"] if rows and rows[0][1] and rows[0][1].get("wakeups") else None

    print("# Idle comparison: %s (qemu-eos, relative numbers only)\n" % cam)
    print("| scenario | wakeups/s | Δ vs canon | IRQ/s | task sw/s | insns/wakeup | busy ratio | ML seen | window s |")
    print("|---|---:|---:|---:|---:|---:|---:|---|---:|")
    for sc, r in rows:
        w = r.get("wakeups") if r else None
        if not w or not w.get("seconds") or (r and r.get("error")):
            why = (r.get("error") or "; ".join(r.get("qemu_errors", []))) if r else "no result"
            print("| %s | FAILED: %s | | | | | | | |" % (sc, why.replace("|", "/")[:120]))
            continue
        delta = (w["wakeups_per_s"] - base["wakeups_per_s"]) if base else None
        print("| %s | %s | %s | %s | %s | %s | %s | %s | %s |" % (
            sc, fmt(w["wakeups_per_s"]), ("%+.1f" % delta) if delta is not None else "n/a",
            fmt(w["irq_per_s"]), fmt(r.get("task_switches_per_s")),
            fmt(w["insns_per_wakeup"], 0), fmt(w["busy_ratio"], 3),
            "yes" if r.get("ml_detected") else "no", fmt(w["seconds"], 1)))
    print()
    for sc, r in rows:
        if r and r.get("top_tasks"):
            print("%s top tasks (switches in window): %s" % (
                sc, ", ".join("%s=%d" % (n, c) for n, c in r["top_tasks"])))
    for sc, r in rows:
        if r and r.get("top_pcs"):
            print("%s PC profile (%d monitor samples): %s" % (
                sc, r["pc_samples"], ", ".join("%s=%.0f%%" % (pc, f * 100) for pc, f in r["top_pcs"][:5])))
    for sc, r in rows:
        if r and "pm_counter_window_end" in r:
            print("%s power save at window end: Canon counter = %s, ML holds = %s; [PM] calls in window = %d "
                  "(%.2f counter changes/s), pmSelfRefresh msgs = %d" % (
                      sc, r.get("pm_counter_window_end"), r.get("ml_holds_window_end"),
                      r.get("pm_calls_in_window", 0), r.get("pm_changes_per_s") or 0, r.get("self_refresh", 0)))
            if r.get("pm_counter_samples"):
                print("    counter samples in window (value=count): %s" % ", ".join(
                    "%s=%d" % kv for kv in sorted(r["pm_counter_samples"].items())))
            if "fio_holds" in r:
                print("    card I/O holds (fio): %d taken, %d released, %d in the window; ML holds after menu "
                      "closed = %s; checks: %s" % (
                          r["fio_holds"], r["fio_releases"], r.get("fio_holds_in_window", 0),
                          r.get("ml_holds_end"), "; ".join(r.get("checks_failed") or []) or "ok"))
            if "pm_calls_menu" in r:
                print("    ML menu open %.0f s after the window: [PM] calls = %d (%.2f/s)" % (
                    r["menu_s"], r["pm_calls_menu"], r["pm_calls_menu"] / r["menu_s"]))
            for l in r.get("pm_tail", []) + r.get("ps_log", [])[:12]:
                print("    %s" % l)
            for n, block in enumerate(r.get("pm_context", [])):
                print("    context of DisablePowerSave #%d in the window:" % (n + 1))
                for l in block:
                    print("      %s" % l)
    fails = deep_sleep(out, rows)
    print("\nwakeups/s = WFI (idle entries) per second; busy ratio = 1 - halted/elapsed host time "
          "(TCG over-estimates it). See docs/EMULATION.md.")
    if fails and os.environ.get("DEEP_SLEEP_ASSERT") == "1":
        print("deep-sleep checks failed: %s" % "; ".join(fails), file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
