#!/usr/bin/env python3
import os, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
from qemu_driver import (boot, find_qemu, kill_qemu, screen_text, sendkey,
                         type_text, wait_for_prompt)

PORT = 4577
ISO = os.path.abspath(os.path.join(HERE, "..", "..", "kernel_v25", "catos.iso"))
DISK = os.path.abspath(os.path.join(HERE, "..", "..", "kernel_v25", "catos_disk.img"))

def run(s, c, wait=1.6):
    type_text(s, c); sendkey(s, "ret", wait); return screen_text(s)

QEMU = "/home/user/bin/qemu-system-i386"
proc, s = boot(QEMU, ISO, PORT, disk=DISK)
try:
    time.sleep(28)
    t = screen_text(s)
    print("[boot screen tail]", t.strip()[-80:])
    # #120 alias replace
    run(s, "alias ll=ls")
    run(s, "alias ll=dir")
    t = run(s, "alias ll")
    print("[#120] alias line:", [x for x in t.splitlines() if "ll" in x])
    alias_ok = ("ll='dir'" in t) and ("lsdir" not in t)

    # #121 touch guard
    run(s, "login user")
    time.sleep(1.0)
    t = run(s, "touch /etc/evilprobe2")
    print("[#121] touch /etc/evilprobe2 ->", "denied" if "denied" in t else "ALLOWED")
    touch_denied = "denied" in t
    t = run(s, "touch /tmp/ok.txt")
    print("[#121] touch /tmp/ok.txt ->", "denied" if "denied" in t else "allowed")
    scratch_ok = "denied" not in t
    run(s, "login root")

    t = run(s, "faultinfo")
    print("[final] no fatal:", "panic" not in t.lower() and "halt" not in t.lower())
    print("ALIAS_FIX_OK" if alias_ok else "ALIAS_FIX_FAIL")
    print("TOUCH_FIX_OK" if touch_denied and scratch_ok else "TOUCH_FIX_FAIL")
finally:
    kill_qemu(proc)
