#!/usr/bin/env python3
import os, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
from qemu_driver import (boot, find_qemu, kill_qemu, screen_text, sendkey,
                         type_text, wait_for_prompt, cmd)

PORT = 4575
ISO = os.path.abspath(os.path.join(HERE, "..", "..", "kernel_v25", "catos.iso"))
DISK = os.path.abspath(os.path.join(HERE, "..", "..", "kernel_v25", "catos_disk.img"))

def run(s, c, wait=1.4):
    type_text(s, c); sendkey(s, "ret", wait); return screen_text(s)

proc, s = boot(find_qemu(None), ISO, PORT, disk=DISK, extra_args=["-usb"])
try:
    assert wait_for_prompt(s, 90)
    t = run(s, "usb")
    print("[usb] boot status:", t.strip()[-70:])
    print("[mon] info usb BEFORE:", cmd(s, "info usb", 1.0).strip()[:300])
    r = cmd(s, "device_add usb-kbd", 1.5)
    print("[mon] device_add raw:", repr(r[:200]))
    print("[mon] info usb AFTER:", cmd(s, "info usb", 1.0).strip()[:300])
    time.sleep(2.0)
    t = run(s, "usb scan")
    print("[usb] scan:", t.strip()[-70:])
    t = run(s, "usb")
    print("[usb] status:", t.strip()[-70:])
    print("[mon] qtree usb:", cmd(s, "info qtree", 1.0).strip()[:400])
finally:
    kill_qemu(proc)
