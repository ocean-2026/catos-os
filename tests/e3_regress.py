#!/usr/bin/env python3
"""e3_regress.py -- E3 regression: per-task syscall staging buffers.

Two on-metal scenarios against the freshly built ISO:

  badptr  -- e3probe.ce badptr hands kernel-space / unmapped pointers to
             file_write / pipe_write (and write).  Every call must return
             a negative error; the kernel must keep running (no ring0 fault
             / system halted banner).
  conc    -- e3probe.ce parent spawns one writer on a pipe, drains 8 KiB
             and verifies every byte matches the known cycle, with
             interleaved console traffic.

Usage:
  python tests/e3_regress.py [catos.iso] [--shot-dir DIR]
"""
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qemu_driver import (boot, find_qemu, force_text, kill_qemu,  # noqa: E402
                         screen_text, sendkey, type_text, wait_for_prompt, cmd)

REPO = os.path.dirname(HERE)
MONITOR_PORT = 4462
CRASH_MARKERS = ("page fault", "kernel fault", "system halted")
SHOT_DIR = os.path.join(REPO, "artifacts", "e3-syscall")


def wait_verdict(sock, tag, timeout=40.0):
    """Wait for the `E3PROBE <tag> ... PASS/FAIL` line, or a ring0 fault."""
    prefix = "E3PROBE %s" % tag
    deadline = time.time() + timeout
    last = ""
    while time.time() < deadline:
        last = screen_text(sock)
        low = last.lower()
        for marker in CRASH_MARKERS:
            if marker in low:
                return False, True, "kernel fault: %s" % last[-220:]
        for line in last.splitlines():
            if prefix in line:
                tail = line[line.index(prefix):]
                if " FAIL" in tail:
                    return False, False, "verdict FAIL: " + tail[-220:]
                if " PASS" in tail:
                    return True, False, "verdict PASS"
        time.sleep(0.4)
    return False, False, "timeout; screen: %s" % last[-220:]


def shoot(sock, name):
    os.makedirs(SHOT_DIR, exist_ok=True)
    ppm = os.path.join(SHOT_DIR, name + ".ppm")
    png = os.path.join(SHOT_DIR, name + ".png")
    if os.path.exists(ppm):
        os.remove(ppm)
    cmd(sock, "screendump %s" % ppm.replace("\\", "/"), 1.0)
    for _ in range(10):
        if os.path.exists(ppm) and os.path.getsize(ppm) > 0:
            break
        time.sleep(0.3)
    if os.path.exists(ppm):
        subprocess.run(["convert", ppm, png], check=False)
        os.remove(ppm)
    return png


def main():
    args = sys.argv[1:]
    iso = None
    shot_dir = None
    while args:
        a = args.pop(0)
        if a == "--shot-dir":
            shot_dir = args.pop(0)
        elif not a.startswith("--"):
            iso = a
    global SHOT_DIR
    if shot_dir:
        SHOT_DIR = shot_dir

    iso = os.path.abspath(iso or os.path.join(REPO, "kernel_v25", "catos.iso"))
    qemu = find_qemu()
    if not qemu:
        print("qemu-system-i386 not found")
        return 2
    if not os.path.isfile(iso):
        print("ISO not found: %s" % iso)
        return 2

    disk = os.path.join(HERE, "qemu_disk_e3.img")
    if not os.path.exists(disk):
        with open(disk, "wb") as fh:
            fh.truncate(64 * 1024 * 1024)

    proc, s = boot(qemu, iso, MONITOR_PORT, disk=disk)
    force_text(s)
    results = []

    def step(name, ok, note):
        results.append((name, ok))
        print("%-4s %-12s %s" % ("PASS" if ok else "FAIL", name, note))

    def do(line, wait=1.0):
        type_text(s, line)
        sendkey(s, "ret", wait)

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

        # 1) poison pointers
        do("clear", 0.6)
        do("run /bin/e3probe.ce badptr", 1.5)
        ok, crashed, note = wait_verdict(s, "BADPTR")
        step("badptr", ok and not crashed, note)
        shoot(s, "e3-badptr")

        # 2) concurrent pipe writers
        do("clear", 0.6)
        do("run /bin/e3probe.ce parent", 1.5)
        ok, crashed, note = wait_verdict(s, "CONC", timeout=60.0)
        step("conc", ok and not crashed, note)
        shoot(s, "e3-conc")
    finally:
        kill_qemu(proc)

    fails = [n for n, ok in results if not ok]
    print("\nsteps=%d ok=%d fail=%d" % (len(results),
                                        len(results) - len(fails), len(fails)))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
