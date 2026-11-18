#!/usr/bin/env python3
"""disk_regress.py -- on-metal regression for the app/kernel decoupling.

The kernel image embeds CE apps only as a FALLBACK.  build/mkdisk.py
produces a FAT32 disk image with /bin and /appdevelop pre-populated
(byte-identical layout to kernel fat_mkfs(), file list shared with
gen_builtin.py).  This test boots that image WITHOUT running `format`
and proves the disk is a real, working application source:

  1. mount          -> the pre-formatted image mounts cleanly
  2. ls /bin        -> apps visible from the disk (hello/cc/vprobe)
  3. run hello.ce   -> a disk-sourced app executes
  4. run vprobe     -> a bigger disk-sourced app exercises cross-page
                       user buffers on the way
  5. fsck           -> FAT #2 mirror consistent on a host-written image

usage:
  python tests/disk_regress.py [catos.iso] [--qemu PATH]
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qemu_driver import (boot as _qemu_boot, find_qemu,  # noqa: E402
                         kill_qemu, screen_text, sendkey, type_text,
                         wait_for_prompt, force_text)

REPO = os.path.dirname(HERE)
KERNEL = os.path.join(REPO, "kernel_v25")
MONITOR_PORT = 4464                 # this driver's own port; never 4444
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
    port = MONITOR_PORT
    if "--port" in args:
        i = args.index("--port")
        if i + 1 < len(args):
            port = int(args[i + 1])

    iso = os.path.abspath(iso or os.path.join(KERNEL, "catos.iso"))
    qemu = find_qemu(qemu)
    if not qemu:
        print("qemu-system-i386 not found")
        return 2
    if not os.path.isfile(iso):
        print("ISO not found: %s" % iso)
        return 2

    # fresh image every run, from the current build outputs
    disk = os.path.join(HERE, "qemu_disk_app.img")
    r = subprocess.run([sys.executable,
                        os.path.join(KERNEL, "build", "mkdisk.py"),
                        "-o", win_path(disk), "-m", "64"],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print("FATAL: mkdisk.py failed:\n%s%s" % (r.stdout, r.stderr))
        return 2
    print(r.stdout.strip())

    proc, s = _qemu_boot(qemu, iso, port, disk=disk)
    results = []

    def step(name, ok, note):
        results.append((name, ok))
        print("%-4s %-16s %s" % ("PASS" if ok else "FAIL", name, note))

    def do(line, wait=1.0):
        type_text(s, line)
        sendkey(s, "ret", wait)

    def clear():
        do("clear", 0.6)

    def drain_pager(wait=0.7, max_steps=10):
        """v2.59: long output is paged; page through until the More bar is
        gone so the tail of the listing (and its count line) is on screen."""
        for _ in range(max_steps):
            if "-- more" not in screen_text(s).lower():
                return
            sendkey(s, "spc", wait)

    try:
        force_text(s)          # v2.56: boot defaults to gfx24; text suite
        if not wait_for_prompt(s, timeout=120.0):
            print("FATAL: shell prompt never appeared")
            return 3

        # 1) pre-formatted disk mounts cleanly
        crashed = any(m in screen_text(s).lower() for m in CRASH_MARKERS)
        step("mount", not crashed, "no format needed, no fault")

        # 2) /bin content is visible from the disk.  30 entries overflow
        # the 25-row screen, so assert on tail entries + the count line.
        clear()
        do("ls /bin", 1.5)
        drain_pager()          # v2.59 pager: walk to the last page
        txt = screen_text(s).lower()
        # v2.45: ls output is sorted alphabetically; check a tail-of-list
        # name plus the entry count line (order-independent).
        ok = ("zhdemo.ce" in txt and "entries" in txt)
        step("ls-bin", ok, "disk-sourced app listing")

        # 3) an app runs straight off the disk
        clear()
        do("run /bin/hello.ce", 2.0)
        ok = "hello from ce!" in screen_text(s).lower()
        step("hello", ok, "run /bin/hello.ce")

        # 4) a heavier disk-sourced app (cross-page buffers too)
        clear()
        do("run /bin/vprobe.ce fr", 2.5)
        txt = screen_text(s).lower()
        ok = "vprobe fr pass" in txt and \
            not any(m in txt for m in CRASH_MARKERS)
        step("vprobe", ok, "run /bin/vprobe.ce fr")

        # 5) host-written FAT#2 mirror is consistent
        clear()
        do("fsck", 1.5)
        ok = "fat mirror ok" in screen_text(s).lower()
        step("fsck", ok, "FAT #2 mirrors FAT #1")
    finally:
        kill_qemu(proc)

    fails = [n for n, ok in results if not ok]
    print("\nsteps=%d ok=%d fail=%d" % (len(results),
                                        len(results) - len(fails), len(fails)))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
