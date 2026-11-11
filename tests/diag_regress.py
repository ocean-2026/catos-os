#!/usr/bin/env python3
"""diag_regress.py -- on-metal regression for the diagnostics pillar (#86).

Before #86 a fault left nothing queryable: ring3 deaths printed one line
and ring0 faults halted with a dump nobody could ask about afterwards.

  1. run /bin/crash.ce kernel  -> task killed by a ring3 page fault,
     system survives (isolation), and the fault is recorded.
  2. faultinfo                 -> shows that fault: vector, task, CR2 and
     the decoded PF error-code bits.
  3. fsck                      -> FAT #2 mirror verified against FAT #1
     after a clean format.

usage:
  python tests/diag_regress.py [catos.iso] [--qemu PATH]
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qemu_driver import (boot as _qemu_boot, find_qemu,  # noqa: E402
                         kill_qemu, screen_text, sendkey, type_text,
                         wait_for_prompt)

REPO = os.path.dirname(HERE)
MONITOR_PORT = 4463                 # this driver's own port; never 4444
CRASH_MARKERS = ("kernel fault", "system halted")


def win_path(p):
    return os.path.abspath(p).replace("\\", "/")


def main():
    args = sys.argv[1:]
    iso = None
    if args and not args[0].startswith("--"):
        iso = args.pop(0)
    qemu = None
    if "--qemu" in args:
        i = args.index("--qemu")
        if i + 1 < len(args):
            qemu = args[i + 1]

    iso = os.path.abspath(iso or os.path.join(REPO, "kernel_v25", "catos.iso"))
    qemu = find_qemu(qemu)
    if not qemu:
        print("qemu-system-i386 not found")
        return 2
    if not os.path.isfile(iso):
        print("ISO not found: %s" % iso)
        return 2

    disk = win_path(os.path.join(HERE, "qemu_disk_diag.img"))
    if not os.path.exists(disk):
        with open(disk, "wb") as fh:
            fh.truncate(64 * 1024 * 1024)

    proc, s = _qemu_boot(qemu, iso, MONITOR_PORT, disk=disk)
    results = []

    def step(name, ok, note):
        results.append((name, ok))
        print("%-4s %-16s %s" % ("PASS" if ok else "FAIL", name, note))

    def do(line, wait=1.0):
        type_text(s, line)
        sendkey(s, "ret", wait)

    def clear():
        do("clear", 0.6)

    try:
        if not wait_for_prompt(s, timeout=120.0):
            print("FATAL: shell prompt never appeared")
            return 3

        type_text(s, "format")
        sendkey(s, "ret", 1.0)
        sendkey(s, "y", 0.4)
        sendkey(s, "ret", 15.0)
        if not wait_for_prompt(s, timeout=90.0):
            print("FATAL: shell never returned after format")
            return 3
        step("format", True, "FAT32 volume ready")

        # 1) a deliberate ring3 fault must be survivable and recorded
        clear()
        do("run /bin/crash.ce kernel", 2.5)
        txt = screen_text(s)
        low = txt.lower()
        crashed = any(m in low for m in CRASH_MARKERS)
        killed = ("killed" in low and "crash" in low)
        step("crash-killed", killed and not crashed,
             "killed=%s kernel_survived=%s" % (killed, not crashed))

        # 2) the shell must be able to query what happened afterwards
        clear()
        do("faultinfo", 1.5)
        txt = screen_text(s)
        # err=0x7 = present | write | user-mode access (CPL=3 store into a
        # supervisor-only page).  Decode lives in faultinfo, not the raw hex.
        ok = ("last fault" in txt and "page-fault" in txt
              and "0x01000100" in txt and "task crash" in txt
              and "present write user" in txt)
        step("faultinfo", ok, txt.strip().splitlines()[-1] if txt.strip() else "")

        # 3) crash-consistency check on a clean volume
        clear()
        do("fsck", 1.5)
        ok2 = "FAT mirror OK" in screen_text(s)
        step("fsck", ok2, "FAT #2 mirrors FAT #1")
    finally:
        kill_qemu(proc)

    fails = [n for n, ok in results if not ok]
    print("\nsteps=%d ok=%d fail=%d" % (len(results),
                                        len(results) - len(fails), len(fails)))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
