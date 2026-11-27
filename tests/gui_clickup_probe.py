#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""A7.2 松开命中触发取证探针：`gui clickup` -> 串口 `GUI:clickup ... result=`。"""
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


def main():
    serial = os.path.join(ART, "serial-clickup.log")
    if os.path.exists(serial):
        os.remove(serial)
    qemu = q.find_qemu(None)
    proc, s = q.boot(qemu, ISO, 4622, disk=VPROBE_DISK,
                     extra_args=["-smp", "1", "-cpu", "qemu32", "-vga", "vmware",
                                 "-serial", "file:" + serial])
    try:
        q.force_text(s, settle=10)
        if not q.wait_for_prompt(s, timeout=90):
            raise RuntimeError("no shell prompt")
        q.type_text(s, "gui clickup", 0.05)
        q.sendkey(s, "ret", 1.0)
        time.sleep(6.0)
        txt = open(serial, "r", encoding="utf-8", errors="replace").read()
        for ln in txt.splitlines():
            if "clickup" in ln:
                print("  [clickup]", ln)
    finally:
        q.kill_qemu(proc)


if __name__ == "__main__":
    main()
