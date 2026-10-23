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
        if r and r.get("pm_calls") is not None:
            print("%s power save: Canon counter (last [PM] msg) = %s over %d calls, ML holds (last [PS] msg) = %s, "
                  "pmSelfRefresh msgs = %d" % (sc, r.get("pm_counter_last"), r.get("pm_calls", 0),
                                               r.get("ml_holds_last"), r.get("self_refresh", 0)))
    print("\nwakeups/s = WFI (idle entries) per second; busy ratio = 1 - halted/elapsed host time "
          "(TCG over-estimates it). See docs/EMULATION.md.")


if __name__ == "__main__":
    main()
