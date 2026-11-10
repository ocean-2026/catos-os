#!/usr/bin/env python3
"""vprobe_regress.py -- on-metal regression for issue #78.

A ring3 CE probe (extapp/vprobe.c -> /bin/vprobe.ce) allocates 16 bytes on
the lazily-mapped heap, then asks syscalls to read or write buffers that
cross into the next, still-unmapped page.  Every syscall used to validate
only the first byte, so a ring0 store/load took an unrecoverable page fault
and halted the whole machine.

Each case is a separate `run` so a crashing case is attributed precisely.
The driver detects the kernel's own fault banner ("ring0 kernel fault")
and stops: once the guest is halted no further case can be meaningful.

usage:
  python tests/vprobe_regress.py [catos.iso] [--qemu PATH] [--only CASE]
"""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qemu_driver import (boot as _qemu_boot, find_qemu,  # noqa: E402
                         kill_qemu, screen_text, sendkey, type_text,
                         wait_for_prompt)

REPO = os.path.dirname(HERE)
MONITOR_PORT = 4461                 # this driver's own port; never 4444
CASES = ["fr", "cat", "hw", "str", "strcpy"]
CRASH_MARKERS = ("page fault", "kernel fault", "system halted")


def win_path(p):
    """QEMU is a native Windows binary and does not understand MSYS /tmp."""
    return os.path.abspath(p).replace("\\", "/")


def wait_verdict(sock, case, timeout=35.0):
    """Wait for `VPROBE <case> PASS`, or a ring0 fault banner."""
    deadline = time.time() + timeout
    last = ""
    ok_line = "VPROBE %s PASS" % case
    bad_line = "VPROBE %s FAIL" % case
    while time.time() < deadline:
        last = screen_text(sock)
        low = last.lower()
        for marker in CRASH_MARKERS:
            if marker in low:
                return False, True, "kernel fault: %s" % last[-220:]
        if ok_line in last:
            return True, False, "verdict PASS"
        if bad_line in last:
            return False, False, "verdict FAIL: %s" % last[-220:]
        time.sleep(0.4)
    return False, False, "timeout; screen: %s" % last[-220:]


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
    only = None
    if "--only" in args:
        i = args.index("--only")
        if i + 1 < len(args):
            only = args[i + 1]
    cases = [only] if only else CASES

    iso = os.path.abspath(iso or os.path.join(REPO, "kernel_v25", "catos.iso"))
    qemu = find_qemu(qemu)
    if not qemu:
        print("qemu-system-i386 not found")
        return 2
    if not os.path.isfile(iso):
        print("ISO not found: %s" % iso)
        return 2

    disk = win_path(os.path.join(HERE, "qemu_disk_vprobe.img"))
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

        # The built-in apps are installed onto the FAT volume by `format`.
        type_text(s, "format")
        sendkey(s, "ret", 1.0)
        sendkey(s, "y", 0.4)
        sendkey(s, "ret", 15.0)
        if not wait_for_prompt(s, timeout=90.0):
            print("FATAL: shell never returned after format")
            return 3
        step("format", True, "FAT32 volume ready")

        clear()
        do("ls /bin", 2.5)
        has = "vprobe.ce" in screen_text(s).lower()
        step("bin-has-vprobe", has, "/bin/vprobe.ce=%s" % has)
        if not has:
            return 1

        halted = False
        for case in cases:
            if halted:
                step(case, False, "guest already halted by a previous case")
                continue
            clear()
            do("run /bin/vprobe.ce %s" % case, 1.5)
            ok, crashed, note = wait_verdict(s, case)
            step(case, ok, note)
            if crashed:
                halted = True
    finally:
        kill_qemu(proc)

    fails = [n for n, ok in results if not ok]
    print("\nsteps=%d ok=%d fail=%d" % (len(results),
                                        len(results) - len(fails), len(fails)))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
