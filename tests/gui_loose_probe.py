#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""A7.6 BOUND_LOOSE 宽松出屏档取证探针：`gui loose` -> `GUI:loose ... result=`。

断言（见 chrome.c gui_probe_loose）：
  - 左上/右下拖出屏后，顶部标题条(24px)仍在屏内；
  - 水平至少留 6px 可见；
  - 左上拖后块左缘为负（确实部分出屏，未被整窗夹紧）。
BOUND_SCREEN / BOUND_PARENT 路径不在本探针覆盖内。
"""
import os
import re
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
    serial = os.path.join(ART, "serial-loose.log")
    if os.path.exists(serial):
        os.remove(serial)
    qemu = q.find_qemu(None)
    proc, s = q.boot(qemu, ISO, 4624, disk=VPROBE_DISK,
                     extra_args=["-smp", "1", "-cpu", "qemu32", "-vga", "vmware",
                                 "-serial", "file:" + serial])
    result = 0
    try:
        q.force_text(s, settle=10)
        if not q.wait_for_prompt(s, timeout=90):
            raise RuntimeError("no shell prompt")
        q.type_text(s, "gui loose", 0.05)
        q.sendkey(s, "ret", 1.0)
        time.sleep(6.0)
        txt = open(serial, "r", encoding="utf-8", errors="replace").read()
        for ln in txt.splitlines():
            if "GUI:loose" in ln:
                print("  [loose]", ln)
                m = re.search(r"result=(\d+)", ln)
                if m:
                    result = int(m.group(1))
    finally:
        q.kill_qemu(proc)
    print("LOOSE_RESULT", result)
    sys.exit(0 if result == 1 else 1)


if __name__ == "__main__":
    main()
