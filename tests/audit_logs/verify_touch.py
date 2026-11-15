#!/usr/bin/env python3
import os, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
from qemu_driver import (boot, find_qemu, kill_qemu, screen_text, sendkey,
                         type_text)

PORT = 4578
QEMU = "/home/user/bin/qemu-system-i386"
ISO = os.path.abspath(os.path.join(HERE, "..", "..", "kernel_v25", "catos.iso"))
DISK = os.path.abspath(os.path.join(HERE, "..", "..", "kernel_v25", "catos_disk.img"))

def run(s, c, wait=1.6):
    type_text(s, c); sendkey(s, "ret", wait); return screen_text(s)

proc, s = boot(QEMU, ISO, PORT, disk=DISK)
try:
    time.sleep(28)
    run(s, "login user"); time.sleep(1.0)
    run(s, "clear")
    run(s, "touch /tmp/okprobe.txt")
    run(s, "ls /tmp")
    t = screen_text(s)
    print("[scratch touch listed]", "okprobe.txt" in t)
    run(s, "clear")
    run(s, "touch /etc/evilprobe3")
    t = screen_text(s)
    print("[etc touch denied]", "denied" in t)
finally:
    kill_qemu(proc)
