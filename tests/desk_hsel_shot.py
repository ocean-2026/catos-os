#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""P1-1 hover/选中态截图取证：desk-hover.png / desk-selected.png / desk-selected-light.png。

  gui shot-hov   悬停条目 0（border 令牌细描边）
  gui shot-sel   选中条目 0（accent 令牌粗描边 + panel 淡底）
  gui shot-sel light  选中态 + light 令牌对版
"""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import qemu_driver as q
from desk_r4_probe import build_disk

ISO = os.path.join(REPO, "kernel_v25", "catos.iso")
ART = os.path.join(REPO, "artifacts", "desktop-icons", "round5")
os.makedirs(ART, exist_ok=True)


def shoot(s, png_name):
    ppm = os.path.join(ART, png_name.replace(".png", ".ppm"))
    png = os.path.join(ART, png_name)
    if os.path.exists(ppm):
        os.remove(ppm)
    q.cmd(s, "screendump " + ppm, 1.0)
    deadline = time.time() + 10
    while time.time() < deadline:
        if os.path.exists(ppm) and os.path.getsize(ppm) > 0:
            break
        time.sleep(0.3)
    from PIL import Image
    Image.open(ppm).convert("RGB").save(png)
    print("SHOT", png)
    return png


def capture(port, cmd, png):
    disk = build_disk("six")
    serial = os.path.join(ART, "serial-hsel-%s.log" % png.replace(".png", ""))
    if os.path.exists(serial):
        os.remove(serial)
    qemu = q.find_qemu(None)
    proc, s = q.boot(qemu, ISO, port, disk=disk,
                     extra_args=["-smp", "1", "-cpu", "qemu32", "-vga", "vmware",
                                 "-serial", "file:" + serial])
    try:
        q.force_text(s, settle=10)
        if not q.wait_for_prompt(s, timeout=90):
            raise RuntimeError("no shell prompt (%s)" % png)
        q.type_text(s, cmd, 0.05)
        q.sendkey(s, "ret", 1.0)
        time.sleep(6.0)
        shoot(s, png)
    finally:
        q.kill_qemu(proc)


def main():
    capture(4626, "gui shot-hov",  "desk-hover.png")
    capture(4627, "gui shot-sel",  "desk-selected.png")
    capture(4628, "gui shot-sel light", "desk-selected-light.png")


if __name__ == "__main__":
    main()
