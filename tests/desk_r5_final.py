#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""R5 桌面审美 P0 修复取证：desk-final（六条目深色）/ desk-final-empty / desk-final-light。

复用 desk_r4_probe 的宿主侧 FAT/LFN 播种。产出落在 artifacts/desktop-icons/round5/。
用法: python3 tests/desk_r5_final.py [six empty light]
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


def capture(scenario, port, png, cmd):
    disk = build_disk(scenario)
    serial = os.path.join(ART, "serial-final-%s.log" % scenario)
    if os.path.exists(serial):
        os.remove(serial)
    qemu = q.find_qemu(None)
    proc, s = q.boot(qemu, ISO, port, disk=disk,
                     extra_args=["-smp", "1", "-cpu", "qemu32", "-vga", "vmware",
                                 "-serial", "file:" + serial])
    try:
        q.force_text(s, settle=10)
        if not q.wait_for_prompt(s, timeout=90):
            raise RuntimeError("no shell prompt (%s)" % scenario)
        q.type_text(s, cmd, 0.05)
        q.sendkey(s, "ret", 1.0)
        time.sleep(6.0)
        shoot(s, png)
    finally:
        q.kill_qemu(proc)


def main():
    which = sys.argv[1:] or ["six", "empty", "light"]
    if "six" in which:
        capture("six", 4621, "desk-final.png", "gui")
    if "empty" in which:
        capture("empty", 4622, "desk-final-empty.png", "gui")
    if "light" in which:
        capture("light", 4623, "desk-final-light.png", "gui shot-light")


if __name__ == "__main__":
    main()
