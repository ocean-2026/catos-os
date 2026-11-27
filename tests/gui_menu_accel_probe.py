#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""A7.1 菜单/浮层字母加速键取证探针。

实机进入桌面 -> `gui menu-accel`（内核探针：Alt+N 开菜单直达 Notepad，
未匹配字母吞键不漂移，菜单打开态普通字母经钩子吞键）-> 截图留下划线可见证据，
串口回打 `GUI:menu-accel ... result=`。
产出落在 artifacts/desktop-icons/round5/。
"""
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
    return png


def main():
    serial = os.path.join(ART, "serial-menu-accel.log")
    if os.path.exists(serial):
        os.remove(serial)
    qemu = q.find_qemu(None)
    proc, s = q.boot(qemu, ISO, 4621, disk=VPROBE_DISK,
                     extra_args=["-smp", "1", "-cpu", "qemu32", "-vga", "vmware",
                                 "-serial", "file:" + serial])
    try:
        q.force_text(s, settle=10)
        if not q.wait_for_prompt(s, timeout=90):
            raise RuntimeError("no shell prompt")
        q.type_text(s, "gui menu-accel", 0.05)
        q.sendkey(s, "ret", 1.0)
        time.sleep(6.0)
        shoot(s, "menu-accel.png")
        txt = open(serial, "r", encoding="utf-8", errors="replace").read()
        for ln in txt.splitlines():
            if "menu-accel" in ln:
                print("  [menu-accel]", ln)
    finally:
        q.kill_qemu(proc)


if __name__ == "__main__":
    main()
