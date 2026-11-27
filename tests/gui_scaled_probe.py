#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""A7.3 最近邻缩放 + 逆映射取证探针：`gui scaled` -> `GUI:scaled ... result=`。"""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import qemu_driver as q

ISO = os.path.join(REPO, "kernel_v25", "catos.iso")
ART = os.path.join(REPO, "artifacts", "desktop-icons", "round5")
VPROBE_DISK = os.path.join(HERE, "qemu_disk_vprobe.img")
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


def main():
    serial = os.path.join(ART, "serial-scaled.log")
    if os.path.exists(serial):
        os.remove(serial)
    qemu = q.find_qemu(None)
    proc, s = q.boot(qemu, ISO, 4623, disk=VPROBE_DISK,
                     extra_args=["-smp", "1", "-cpu", "qemu32", "-vga", "vmware",
                                 "-serial", "file:" + serial])
    try:
        q.force_text(s, settle=10)
        if not q.wait_for_prompt(s, timeout=90):
            raise RuntimeError("no shell prompt")
        q.type_text(s, "gui scaled", 0.05)
        q.sendkey(s, "ret", 1.0)
        time.sleep(6.0)
        shoot(s, "scaled-fill.png")
        txt = open(serial, "r", encoding="utf-8", errors="replace").read()
        for ln in txt.splitlines():
            if "scaled" in ln:
                print("  [scaled]", ln)
    finally:
        q.kill_qemu(proc)


if __name__ == "__main__":
    main()
