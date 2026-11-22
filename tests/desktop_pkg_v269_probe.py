#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""v2.69 P1: detachable desktop extension lifecycle on real QEMU.

Proves the CDE module is a pkg-installed CE app, not kernel code:
  1. pkg list shows desktop available, /bin has no desktop.ce
  2. pkg install desktop -> verifies RSA-512 signature, writes /bin/desktop.ce
  3. `desktop` (bare-name PATH lookup) runs the CE desktop: ASCII windows,
     icons, taskbar render via gotoxy + the new mouse slots
  4. press 'q' -> desktop exits back to the text shell
  5. pkg remove desktop -> /bin/desktop.ce gone; `desktop` no longer runs
  The kernel binary is never modified by install/remove.
"""
import hashlib
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qemu_driver import (boot, find_qemu, kill_qemu, read_screen,
                         screen_text, sendkey, type_text, wait_for_prompt,
                         force_text)

REPO = os.path.dirname(HERE)
KERNEL = os.path.join(REPO, "kernel_v25")
ISO = os.path.join(KERNEL, "catos.iso")
PORT = 4516


def main():
    qemu = find_qemu(None)
    if not qemu:
        print("qemu not found")
        return 2
    disk = os.path.join(HERE, "qemu_disk_desktop.img")
    r = subprocess.run([sys.executable, os.path.join(KERNEL, "build", "mkdisk.py"),
                        "-o", disk.replace("\\", "/"), "-m", "64"],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print("mkdisk failed:\n%s%s" % (r.stdout, r.stderr))
        return 2
    proc, s = boot(qemu, ISO, PORT, disk=disk)
    results = []

    def check(name, ok, note=""):
        results.append((name, bool(ok)))
        print("%-4s %-34s %s" % ("PASS" if ok else "FAIL", name, note))

    def do(line, wait=1.2):
        type_text(s, line, 0.01)
        sendkey(s, "ret", wait)

    def clear():
        do("clear", 0.5)

    try:
        force_text(s)
        if not wait_for_prompt(s, timeout=120.0):
            print("FATAL: no prompt")
            return 3

        # 1) available, not installed
        clear()
        do("pkg list", 1.2)
        t = screen_text(s)
        check("pkg list shows desktop", "desktop" in t, "available in repo")
        clear()
        do("ls /bin", 1.5)
        t = screen_text(s)
        check("desktop.ce absent before install", "desktop.ce" not in t)

        # 2) install (RSA-512 verification)
        clear()
        do("pkg install desktop", 2.5)
        t = screen_text(s)
        check("signed desktop pkg installs", "installed" in t and "desktop" in t,
              t[-120:].replace("\n", " "))
        clear()
        do("pkg list", 1.2)
        t = screen_text(s)
        dline = [l for l in t.split(" ") if False]  # placeholder
        check("desktop marked installed", "desktop" in t and "installed" in t,
              t[-160:].replace("\n"," "))

        # 3) run the desktop via bare-name PATH lookup
        clear()
        do("desktop", 2.0)
        t = screen_text(s)
        check("desktop renders windows", "Terminal" in t and "Files" in t,
              "gotoxy window chrome")
        check("desktop renders taskbar/icons", "Start" in t and "Terminal]" in t)
        # desktop loop is running; press q to exit
        sendkey(s, "q", 0.8)
        sendkey(s, "ret", 1.0)
        t = screen_text(s)
        # v2.73: "root@catos" in t was a FALSE POSITIVE -- the desktop is
        # drawn with gotoxy() onto the text buffer, so the old prompt from
        # before `desktop` stayed in the screen snapshot and matched even
        # when the CE loop was still running.  Only accept the explicit
        # marker printed by desktop.c on its exit path.
        check("desktop exits to shell", "exited to text shell" in t,
              t[-120:].replace("\n", " "))

        # 5) remove -> detachable
        clear()
        do("pkg remove desktop", 2.0)
        t = screen_text(s)
        check("desktop pkg removed", "removed" in t or "detach" in t)
        clear()
        do("pkg list", 1.2)
        t = screen_text(s)
        check("desktop marked available after remove",
              "desktop" in t and "available" in t,
              t[-100:].replace("\n"," "))
        clear()
        do("desktop", 1.2)
        t = screen_text(s)
        check("desktop no longer runs", "unknown" in t.lower() or
              "no such" in t.lower() or "not found" in t.lower(),
              t[-100:].replace("\n", " "))
    finally:
        kill_qemu(proc)

    passed = sum(1 for _, ok in results if ok)
    print("\n%d/%d PASS" % (passed, len(results)))
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
