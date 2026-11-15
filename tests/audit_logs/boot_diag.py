#!/usr/bin/env python3
import os, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
from qemu_driver import boot, kill_qemu, screen_text, read_screen, wait_for_prompt
import re

PORT = 4579
QEMU = "/home/user/bin/qemu-system-i386"
ISO = os.path.abspath(os.path.join(HERE, "..", "..", "kernel_v25", "catos.iso"))
DISK = os.path.abspath(os.path.join(HERE, "..", "..", "kernel_v25", "catos_disk.img"))

proc, s = boot(QEMU, ISO, PORT, disk=None)
try:
    time.sleep(25)
    raw = read_screen(s)
    print("=== FULL RAW ===")
    print(repr(raw))
    pat = re.compile(r"catos:[^\n]*\$")
    print("=== regex match:", bool(pat.search(raw)))
    for ln in raw.splitlines():
        if "$" in ln or "catos:" in ln:
            print("LINE>", repr(ln))
finally:
    kill_qemu(proc)
