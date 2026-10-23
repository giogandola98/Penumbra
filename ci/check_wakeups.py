#!/usr/bin/env python3
"""List the ML tasks linked into a build and fail if a forbidden one is present.

Every TASK_CREATE() places a `task_create_<entry>` record in the `.tasks`
section (between `_tasks_start` and `_tasks_end`); init.c starts each of them
at boot, so a record there means a task that wakes up for the whole session.

usage: check_wakeups.py ELF [--nm arm-none-eabi-nm] [--forbid fps_task ...]
"""
import argparse
import subprocess
import sys


def linked_tasks(elf, nm):
    syms = {}
    for line in subprocess.run([nm, elf], capture_output=True, text=True,
                               check=True).stdout.splitlines():
        parts = line.split()
        if len(parts) == 3:
            syms[parts[2]] = int(parts[0], 16)
    start, end = syms["_tasks_start"], syms["_tasks_end"]
    return sorted(name[len("task_create_"):] for name, addr in syms.items()
                  if name.startswith("task_create_") and start <= addr < end)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("--nm", default="arm-none-eabi-nm")
    ap.add_argument("--forbid", nargs="*", default=[])
    args = ap.parse_args()

    tasks = linked_tasks(args.elf, args.nm)
    print("%d boot tasks: %s" % (len(tasks), " ".join(tasks)))
    bad = sorted(set(tasks) & set(args.forbid))
    if bad:
        print("forbidden tasks linked: " + " ".join(bad))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
