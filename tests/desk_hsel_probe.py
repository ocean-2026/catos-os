#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""P1-1 桌面 hover/选中态功能自检：`gui hsel` -> `GUI:hsel ... result=`。

断言：
  hov_in=1   悬停命中落在条目 0 的子树内；
  sel1=0     单击条目 0 后选中索引为 0；
  sel2<0     点桌面空白后选中取消（sel=-1）。
命中链与单击打开契约保持既有实现，本脚本只读串口结果。
"""
import os
import re
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


def main():
    disk = build_disk("six")
    serial = os.path.join(ART, "serial-hsel.log")
    if os.path.exists(serial):
        os.remove(serial)
    qemu = q.find_qemu(None)
    proc, s = q.boot(qemu, ISO, 4625, disk=disk,
                     extra_args=["-smp", "1", "-cpu", "qemu32", "-vga", "vmware",
                                 "-serial", "file:" + serial])
    result = 0
    try:
        q.force_text(s, settle=10)
        if not q.wait_for_prompt(s, timeout=90):
            raise RuntimeError("no shell prompt")
        q.type_text(s, "gui hsel", 0.05)
        q.sendkey(s, "ret", 1.0)
        time.sleep(6.0)
        txt = open(serial, "r", encoding="utf-8", errors="replace").read()
        for ln in txt.splitlines():
            if "GUI:hsel" in ln:
                print("  [hsel]", ln)
                m = re.search(r"result=(\d+)", ln)
                if m:
                    result = int(m.group(1))
    finally:
        q.kill_qemu(proc)
    print("HSEL_RESULT", result)
    sys.exit(0 if result == 1 else 1)


if __name__ == "__main__":
    main()
