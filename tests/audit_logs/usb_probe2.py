#!/usr/bin/env python3
import os, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
from qemu_driver import (boot, find_qemu, kill_qemu, screen_text, sendkey,
                         type_text, wait_for_prompt, cmd)

PORT = 4576
ISO = os.path.abspath(os.path.join(HERE, "..", "..", "kernel_v25", "catos.iso"))
DISK = os.path.abspath(os.path.join(HERE, "..", "..", "kernel_v25", "catos_disk.img"))

mode = sys.argv[1] if len(sys.argv) > 1 else "boot"
extra = ["-usb"]
if mode == "boot":
    extra += ["-usbdevice", "keyboard"]

proc, s = boot(find_qemu(None), ISO, PORT, disk=DISK, extra_args=extra)
try:
    ok = wait_for_prompt(s, 60)
    print("[prompt]", ok)
    t = screen_text(s)
    print("[boot banner]", t.strip().splitlines()[:8])
    if mode == "hot":
        cmd(s, "device_add usb-kbd", 1.5)
        time.sleep(2.0)
    type_text(s, "usb"); sendkey(s, "ret", 1.5)
    print("[usb]", screen_text(s).strip()[-120:])
finally:
    kill_qemu(proc)
