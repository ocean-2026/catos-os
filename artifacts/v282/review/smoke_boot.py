#!/usr/bin/env python3
# THROWAWAY review smoke script (not product code): boot release ISO on a
# fresh scratch disk, capture the gfx24 boot frame, switch to text to read the
# version banner via `about`, then enter the block desktop and capture it.
import os, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))   # .../CatOs
sys.path.insert(0, os.path.join(REPO, "tests"))
from qemu_driver import (boot, find_qemu, kill_qemu, screen_text,
                         sendkey, type_text, force_text)
from qemu_regress import ppm_to_png

ISO = os.path.join(REPO, "kernel_v25", "catos-v2.82.iso")
DISK = os.path.join(HERE, "scratch-smoke.img")
PORT = 4477

def shoot(ppm, png):
    if os.path.exists(ppm):
        os.remove(ppm)
    for _ in range(2):
        s.sendall(("screendump %s\n" % ppm).encode())
        time.sleep(1.6)
    if os.path.exists(ppm):
        ppm_to_png(ppm, png)

# fresh 64 MiB scratch volume (kernel formats it on first boot)
with open(DISK, "wb") as fh:
    fh.truncate(64 * 1024 * 1024)

qemu = find_qemu(None)
print("qemu:", qemu)
p, s = boot(qemu, ISO, PORT, disk=DISK)
try:
    time.sleep(18.0)                      # settle in default gfx24 shell
    shoot(os.path.join(HERE, "01-boot-frame.ppm"),
          os.path.join(HERE, "01-boot-frame.png"))

    # switch to text so read_screen (0xB8000) can read the banner
    force_text(s, settle=6.0)
    time.sleep(1.0)
    type_text(s, "about")
    sendkey(s, "ret", 2.0)
    time.sleep(1.5)
    txt = screen_text(s)
    with open(os.path.join(HERE, "about-screen.txt"), "w") as fh:
        fh.write(txt)
    shoot(os.path.join(HERE, "01b-about-text.ppm"),
          os.path.join(HERE, "01b-about-text.png"))

    # back to gfx24, then block desktop
    type_text(s, "gfx gfx24")
    sendkey(s, "ret", 3.0)
    time.sleep(2.0)
    type_text(s, "desktop")
    sendkey(s, "ret", 4.0)
    time.sleep(4.0)
    shoot(os.path.join(HERE, "02-desktop-frame.ppm"),
          os.path.join(HERE, "02-desktop-frame.png"))

    print("=== ABOUT TEXT ===")
    print(txt)
finally:
    kill_qemu(p)
