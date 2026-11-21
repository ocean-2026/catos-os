#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""v2.69 regression: CE ABI slot 65 gotoxy(x,y) forwards both arguments.

Before the fix nargs[65] was implicitly 0, so the vsyscall thunk did not
load ebx/ecx and gotoxy() saw stale registers -> text landed in the wrong
cell (or the syscall errored).  gotoprobe.ce clears the screen, calls
gotoxy(10,3) and writes a marker.  We read the raw VGA text buffer and
assert the marker is exactly at row 3, col 10.
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qemu_driver import (boot, find_qemu, kill_qemu, read_screen,
                         sendkey, type_text, wait_for_prompt, force_text)

REPO = os.path.dirname(HERE)
KERNEL = os.path.join(REPO, "kernel_v25")
CE = os.path.join(KERNEL, "extapp", "bin", "gotoprobe.ce")
PORT = 4512
MARK = "[GOTO-WORD]"


def main():
    qemu = find_qemu(None)
    if not qemu:
        print("qemu not found")
        return 2
    if not os.path.isfile(CE):
        print("CE not built: %s" % CE)
        return 2
    iso = os.path.join(KERNEL, "catos.iso")
    disk = os.path.join(HERE, "qemu_disk_gotoxy.img")
    r = subprocess.run([sys.executable, os.path.join(KERNEL, "build", "mkdisk.py"),
                        "-o", disk.replace("\\", "/"), "-m", "64"],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print("mkdisk failed:\n%s%s" % (r.stdout, r.stderr))
        return 2
    subprocess.run([sys.executable, os.path.join(HERE, "inject_any.py"),
                    disk, CE, "GOTOPROBE.CE"], check=True,
                   capture_output=True, text=True)
    proc, s = boot(qemu, iso, PORT, disk=disk)
    results = []

    def check(name, ok, note=""):
        results.append((name, bool(ok)))
        print("%-4s %-24s %s" % ("PASS" if ok else "FAIL", name, note))

    try:
        force_text(s)
        if not wait_for_prompt(s, timeout=120.0):
            print("FATAL: no prompt")
            return 3
        type_text(s, "run /bin/gotoprobe.ce")
        sendkey(s, "ret", 1.5)
        raw = read_screen(s)
        rows = raw.splitlines()
        row3 = rows[3] if len(rows) > 3 else ""
        cell = row3[10:10 + len(MARK)]
        print("row3 = %r" % row3)
        print("cell(10,3) = %r" % cell)
        check("gotoxy marker at (10,3)", cell == MARK,
              "expected %r got %r" % (MARK, cell))
        check("gotoxy rc hex at (0,0)", rows[0][:1] == "0",
              "expected '0' got %r" % rows[0][:1])
    finally:
        kill_qemu(proc)
    passed = sum(1 for _, ok in results if ok)
    print("\n%d/%d PASS" % (passed, len(results)))
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
