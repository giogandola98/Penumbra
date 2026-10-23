#!/usr/bin/env python3
"""Markdown comparison table of the scenario results written by scenario.py.

Usage: compare.py CAM OUT_DIR SCENARIO...
Deltas are relative to the first scenario (the Canon-only baseline).
"""
import json
import os
import sys


def load(out, sc):
    try:
        with open(os.path.join(out, sc, "result.json")) as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def fmt(v, nd=1):
    return "n/a" if v is None else ("%.*f" % (nd, v))


def main():
    cam, out, scenarios = sys.argv[1], sys.argv[2], sys.argv[3:]
    rows = [(sc, load(out, sc)) for sc in scenarios]
    base = rows[0][1]["wakeups"] if rows and rows[0][1] and rows[0][1].get("wakeups") else None

    print("# Idle comparison: %s (qemu-eos, relative numbers only)\n" % cam)
    print("| scenario | wakeups/s | Δ vs canon | IRQ/s | task sw/s | insns/wakeup | busy ratio | ML seen | window s |")
    print("|---|---:|---:|---:|---:|---:|---:|---|---:|")
    for sc, r in rows:
        w = r.get("wakeups") if r else None
        if not w or not w.get("seconds"):
            print("| %s | FAILED | | | | | | | |" % sc)
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
    print("\nwakeups/s = WFI (idle entries) per second; busy ratio = 1 - halted/elapsed host time "
          "(TCG over-estimates it). See docs/EMULATION.md.")


if __name__ == "__main__":
    main()
