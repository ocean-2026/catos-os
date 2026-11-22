#!/usr/bin/env python3
"""Prototype probe: CE-side gfx24 text rendering (gfxtext.ce).

Boots the clone ISO, injects gfxtext.ce, runs it, and grabs a PPM
screendump of the 24bpp framebuffer for visual glyph inspection.
"""
import os, sys, subprocess
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qemu_driver import (boot, find_qemu, kill_qemu, screen_text,
                         sendkey, type_text, wait_for_prompt, force_text)
from gfx24_regress import hmp

KERNEL = os.path.join(os.path.dirname(HERE), "kernel_v25")
CE  = os.path.join(KERNEL, "extapp", "bin", "gfxtext.ce")
ISO = os.path.join(KERNEL, "catos.iso")
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 4580
SHOT = os.path.join(HERE, "dumps", "gfxtext.ppm")
PY = sys.executable
disk = os.path.join(HERE, "qemu_disk_gfxtext.img")

subprocess.run([PY, os.path.join(KERNEL, "build", "mkdisk.py"), "-o", disk, "-m", "64"],
               check=True, capture_output=True)
subprocess.run([PY, os.path.join(HERE, "inject_any.py"), disk, CE, "GFXTEXT.CE"],
               check=True, capture_output=True)

qemu = find_qemu(None)
p, s = boot(qemu, ISO, PORT, disk=disk)

def do(line, wait=1.0):
    type_text(s, line, 0.01); sendkey(s, "ret", wait)

try:
    force_text(s)
    if not wait_for_prompt(s, timeout=120.0):
        print("FATAL no prompt"); sys.exit(3)
    do("run /bin/gfxtext.ce", 2.0)
    print("TEXT RESULT:", screen_text(s).replace("\n", " ")[-160:])
    os.makedirs(os.path.dirname(SHOT), exist_ok=True)
    if os.path.isfile(SHOT): os.remove(SHOT)
    win = SHOT.replace("/", "\\")
    for _ in range(2):
        hmp(s, "screendump " + win, 1.0)
    print("PPM EXISTS:", os.path.isfile(SHOT),
          os.path.getsize(SHOT) if os.path.isfile(SHOT) else 0)
finally:
    kill_qemu(p)
