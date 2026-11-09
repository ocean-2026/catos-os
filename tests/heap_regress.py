#!/usr/bin/env python3
"""heap_regress.py -- on-metal acceptance for the P0-1 / P0-2 heap fixes.

Two CE probes are installed in the kernel image as built-ins:

  /bin/allocprobe.ce  (V-5)  proves api->alloc() returns a usable,
                               non-NULL address inside the heap window
  /bin/heaptest.ce    (V-6)  proves an allocation may cross page
                               boundaries without a #PF

Both print their verdict as the LAST line of their output, so the check
is a plain substring search on the text screen.

The screen is read through the QEMU monitor's physical-memory dump of
the VGA text buffer ("xp /4000 0xb8000") instead of being OCR-ed from a
screenshot -- the frame buffer holds (char, attribute) pairs, so the
character is every second byte.

The QEMU lifecycle lives in tests/qemu_driver.py.  Two rules matter on
a shared machine:

  * the guest is torn down BY PID, never by image name -- a global
    `taskkill /IM qemu-system-i386.exe` also kills the guests of other
    drivers running concurrently (see qemu_driver.kill_qemu);
  * the shell's screen-clear command is `clear`, not `cls`.

usage:
  python tests/heap_regress.py <catos.iso> [--qemu PATH] [--keep]
"""
import os
import re
import socket
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_driver import (clear_screen, cmd, find_qemu, kill_qemu,  # noqa: E402
                          read_screen, screen_text, sendkey, type_text)

HERE = os.path.dirname(os.path.abspath(__file__))
MONITOR = ("127.0.0.1", 4444)
VGA_TEXT = 0xB8000
VGA_COLS = 80
VGA_ROWS = 25

def boot(iso, qemu, disk):
    """Start the guest and connect to its monitor.

    Returns (proc, sock): the caller must keep `proc` and hand it to
    kill_qemu() so that no OTHER qemu on this machine is disturbed.
    """
    kill_qemu()                 # no-op: nothing of ours is running yet
    time.sleep(1)
    cmdline = [qemu, "-m", "256", "-no-reboot", "-net", "none",
               "-cdrom", iso, "-boot", "order=d",
               "-display", "none",
               "-monitor", "tcp:%s:%d,server,nowait" % MONITOR]
    if disk:
        cmdline += ["-drive", "file=%s,format=raw,if=ide" % disk]
    print("boot:", " ".join(cmdline))
    proc = subprocess.Popen(cmdline, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    for _ in range(20):
        try:
            return proc, socket.create_connection(MONITOR, timeout=5)
        except OSError:
            time.sleep(2)
    kill_qemu(proc)
    raise RuntimeError("could not reach the QEMU monitor")












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
    if iso is None:
        iso = os.path.join(os.path.dirname(HERE), "kernel_v25", "catos.iso")
    iso = os.path.abspath(iso)
    qemu = find_qemu(qemu)
    if not qemu:
        print("qemu-system-i386 not found")
        return 2

    disk = os.path.join(HERE, "qemu_disk.img")
    if not os.path.exists(disk):
        with open(disk, "wb") as fh:
            fh.truncate(64 * 1024 * 1024)

    proc, s = boot(iso, qemu, disk)
    print("monitor connected")
    time.sleep(25)                      # boot to shell (TCG is slow)

    results = []

    def step(name, fn):
        try:
            ok, note = fn()
            results.append((name, ok, note))
            print("%-4s %-20s %s" % ("PASS" if ok else "FAIL", name, note))
        except Exception as e:
            results.append((name, False, str(e)))
            print("%-4s %-20s %s" % ("ERR ", name, e))

    def do(line, wait=1.5):
        type_text(s, line)
        sendkey(s, "ret", wait)

    def clear():
        # `clear`, not `cls`: the CatOS shell has no `cls` command, so
        # using it leaves the previous screen contents in place and a
        # later assertion can match stale text.
        clear_screen(s, 1.0)

    def step_format():
        clear()
        do("format", 1.0)
        sendkey(s, "y", 0.4)
        sendkey(s, "ret", 10.0)          # fs_format(64 MiB) + install
        return True, "formatted"

    def step_ls_bin():
        clear()
        do("ls /bin", 2.0)
        txt = screen_text(s)
        # The FAT volume stores 8.3 names and the shell prints them back
        # through fat_name_show(): the base is truncated to 8 characters
        # and lower-cased.  "allocprobe.ce" therefore shows up as
        # "allocpro.ce" -- matching the long spelling would be wrong.
        ok = "allocpro.ce" in txt and "heaptest.ce" in txt
        return ok, "allocpro.ce=%s heaptest.ce=%s" % (
            "allocpro.ce" in txt, "heaptest.ce" in txt)

    def run_app(argv_line, expect, wait=6.0):
        clear()
        do(argv_line, wait)
        txt = screen_text(s)
        missing = [e for e in expect if e not in txt]
        return (not missing), ("missing=%r" % missing if missing else "all found")

    def step_allocprobe():
        return run_app("run /bin/allocprobe.ce",
                       ["ALLOCPROBE: alloc works"], wait=6.0)

    def step_heaptest():
        return run_app("run /bin/heaptest.ce",
                       ["HEAPTEST PASS"], wait=10.0)

    step("format", step_format)
    step("ls-bin", step_ls_bin)
    step("allocprobe", step_allocprobe)
    step("heaptest", step_heaptest)

    if "--keep" not in args:
        kill_qemu(proc)
    fails = [r for r in results if not r[1]]
    print("\nsteps=%d ok=%d fail=%d" % (len(results),
                                        len(results) - len(fails), len(fails)))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
