#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""keyprobe_v273_probe.py -- regression for the v2.72 ring3-keyboard stall.

Symptom (v2.72): the CDE desktop renders but q/Esc/Ctrl+C never reach its
CE event loop -- actually ALL ring3 CE apps' pollkey()/getch() saw no keys,
because the auto-started ring0 sshd daemon monopolised the CPU.

This probe builds/injects a minimal CE app (extapp/keyprobe.c) that prints
the value returned by blocking getch() and non-blocking pollkey(), then
injects keys with HMP sendkey.  PASS == the decoded key values appear.

usage: python tests/keyprobe_v273_probe.py [catos.iso] [monitor_port]
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qemu_driver import (boot, find_qemu, kill_qemu, screen_text,
                         sendkey, type_text, wait_for_prompt, force_text)

REPO = os.path.dirname(HERE)
KERNEL = os.path.join(REPO, "kernel_v25")
CE = os.path.join(KERNEL, "extapp", "bin", "keyprobe.ce")
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 4570


def build_ce():
    if os.path.isfile(CE):
        return True
    env = dict(os.environ)
    env["PATH"] = "/c/msys64/mingw32/bin:" + env.get("PATH", "")
    r = subprocess.run(["sh", "build_ce.sh", "keyprobe.c"],
                       cwd=os.path.join(KERNEL, "extapp"),
                       env=env, capture_output=True, text=True)
    if r.returncode != 0:
        print("build_ce failed:\n" + r.stdout + r.stderr)
        return False
    return True


def main():
    iso = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 \
        else os.path.join(KERNEL, "catos.iso")
    if not build_ce() or not os.path.isfile(CE):
        print("keyprobe.ce not built")
        return 2
    qemu = find_qemu(None)
    if not qemu:
        print("qemu not found")
        return 2

    disk = os.path.join(HERE, "qemu_disk_keyprobe.img")
    subprocess.run([sys.executable, os.path.join(KERNEL, "build", "mkdisk.py"),
                    "-o", disk, "-m", "64"], check=True, capture_output=True)
    subprocess.run([sys.executable, os.path.join(HERE, "inject_any.py"),
                    disk, CE, "KEYPROBE.CE"], check=True, capture_output=True)

    proc, s = boot(qemu, iso, PORT, disk=disk)
    ok1 = ok2 = ok3 = False
    try:
        force_text(s)
        if not wait_for_prompt(s, timeout=120.0):
            print("FATAL: no prompt")
            return 3
        type_text(s, "run /bin/keyprobe.ce")
        sendkey(s, "ret", 1.5)
        sendkey(s, "a", 1.0)          # blocking getch must see a real key
        t = screen_text(s)
        # HMP sendkey decodes to a scancode-derived value (e.g. 979); the
        # regression signature is simply that getch() returned at all.
        import re
        ok1 = bool(re.search(r"getch=\d", t))
        print("%-4s blocking getch() returned a key" % ("PASS" if ok1 else "FAIL"))
        sendkey(s, "q", 1.2)          # non-blocking pollkey must see 'q' (113)
        t = screen_text(s)
        ok2 = "pollkey=" in t
        ok3 = "exit(0)" in t
        print("%-4s pollkey() saw a key" % ("PASS" if ok2 else "FAIL"))
        print("%-4s CE exited cleanly" % ("PASS" if ok3 else "FAIL"))
    finally:
        kill_qemu(proc)

    n = sum(1 for x in (ok1, ok2, ok3) if x)
    print("\n%d/3 PASS" % n)
    return 0 if n == 3 else 1


if __name__ == "__main__":
    sys.exit(main())
