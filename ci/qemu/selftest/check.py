#!/usr/bin/env python3
"""Assertions on the JSON lines written by the wakeups plugin for wfi_test.S."""
import json
import sys

N_WFI = 200                 # wfi_test.S
# Instructions per cycle in wfi_test.S: WFI 1 + unmask 3 + IRQ entry/handler 10 + re-mask 2
# + ldr 1 + busy 2*1000 + loop 2 = 2019. The summary also holds a few setup/exit instructions.
INSNS_PER_WAKEUP = (2000, 2040)


def load(path):
    with open(path) as f:
        return [json.loads(line) for line in f if line.strip()]


def main():
    case, path = sys.argv[1], sys.argv[2]
    recs = load(path)
    summ = [r for r in recs if r["type"] == "summary"]
    errors = []

    def expect(cond, msg):
        if not cond:
            errors.append(msg)

    expect(len(summ) == 1, "exactly one summary record, got %d" % len(summ))
    if not summ:
        return report(errors)
    s = summ[0]

    if case == "run2":
        expect(s.get("started") is False, "window must not have started: %r" % s)
        return report(errors)

    expect(s["wfi"] == N_WFI, "wfi == %d (got %d)" % (N_WFI, s["wfi"]))
    expect(s["irq"] == N_WFI, "irq == %d (got %d)" % (N_WFI, s["irq"]))
    expect(s["fiq"] == 0, "fiq == 0 (got %d)" % s["fiq"])
    lo, hi = INSNS_PER_WAKEUP
    expect(lo <= s["insns_per_wakeup"] <= hi,
           "insns_per_wakeup in [%d, %d] (got %.1f)" % (lo, hi, s["insns_per_wakeup"]))
    expect(lo <= s["max_insns_between_wfi"] <= hi,
           "max_insns_between_wfi in [%d, %d] (got %d)" % (lo, hi, s["max_insns_between_wfi"]))
    # Most WFIs really sleep (the 1 ms timer is far slower than the ~2k instruction cycle).
    if case != "run3":      # -icount (single-threaded TCG) reports no idle/resume events
        expect(s["halts"] >= N_WFI // 2, "halts >= %d (got %d)" % (N_WFI // 2, s["halts"]))
    expect(0.0 <= s["busy_ratio"] <= 1.0, "busy_ratio in [0, 1] (got %r)" % s["busy_ratio"])
    expect(s["wakeups_per_s"] > 0, "wakeups_per_s > 0")

    if case == "run1":
        ivl = [r for r in recs if r["type"] == "interval"]
        expect(len(ivl) >= 1, "at least one interval record with interval_ms=50")
        expect(sum(r["wfi"] for r in ivl) <= N_WFI, "interval wfi sum <= total")
        expect(s["busy_model"] == "host_time", "busy_model host_time")
    if case == "run3":
        expect(s["busy_model"] == "icount", "busy_model icount")
        # 2019 insns * 16 ns = ~32 us busy per 1 ms tick: a few percent.
        expect(s["busy_ratio"] < 0.2, "icount busy_ratio < 0.2 (got %r)" % s["busy_ratio"])
    return report(errors)


def report(errors):
    for e in errors:
        print("  check failed: " + e)
    sys.exit(1 if errors else 0)


if __name__ == "__main__":
    main()
