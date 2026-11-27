#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""R5 取证探针：A 标题栏 / B 拖拽持久化 / C 双击 .ce / D1 空桌面 / D3 browser audit / D4 终端隔离。

复用 desk_r4_probe 的宿主侧 FAT/LFN 播种。产出落在 artifacts/desktop-icons/round5/。
用法:
  python3 tests/desk_r5_probe.py [which ...]
"""
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import qemu_driver as q
from desk_r4_probe import build_disk, SIX

ISO = os.path.join(REPO, "kernel_v25", "catos.iso")
ART = os.path.join(REPO, "artifacts", "desktop-icons", "round5")
GATES = os.path.join(ART, "gates")
os.makedirs(ART, exist_ok=True)
os.makedirs(GATES, exist_ok=True)
VPROBE_DISK = os.path.join(HERE, "qemu_disk_vprobe.img")


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


def boot(disk, port, serial):
    qemu = q.find_qemu(None)
    proc, s = q.boot(qemu, ISO, port, disk=disk,
                     extra_args=["-smp", "1", "-cpu", "qemu32", "-vga", "vmware",
                                 "-serial", "file:" + serial])
    q.force_text(s, settle=10)
    if not q.wait_for_prompt(s, timeout=90):
        raise RuntimeError("no shell prompt")
    return proc, s


def run_gui_cmd(s, cmd, wait=6.0):
    q.type_text(s, cmd, 0.05)
    q.sendkey(s, "ret", 1.0)
    time.sleep(wait)


def serial_tail(serial, needle):
    if not os.path.exists(serial):
        return None
    txt = open(serial, "r", encoding="utf-8", errors="replace").read()
    for ln in txt.splitlines():
        if needle in ln:
            return ln
    return None


def plain_desktop(which, disk, port, serial, png):
    proc, s = boot(disk, port, serial)
    try:
        run_gui_cmd(s, "gui", 6.0)
        shoot(s, png)
    finally:
        q.kill_qemu(proc)


def r5desk():
    disk = build_disk("six")
    serial = os.path.join(ART, "serial-r5desk.log")
    if os.path.exists(serial):
        os.remove(serial)
    proc, s = boot(disk, 4611, serial)
    try:
        run_gui_cmd(s, "gui r5desk", 6.0)
        shoot(s, "desk-drag-after.png")
        ln = serial_tail(serial, "GUI:r5desk")
        print("  [r5desk]", ln)
    finally:
        q.kill_qemu(proc)
    # 重开：同一磁盘镜像，持久层应已落盘
    proc, s = boot(disk, 4612, serial + ".reopen")
    try:
        run_gui_cmd(s, "gui", 6.0)
        shoot(s, "desk-drag-reopen.png")
    finally:
        q.kill_qemu(proc)


def r5plain_six():
    disk = build_disk("six")
    serial = os.path.join(ART, "serial-six.log")
    plain_desktop("six", disk, 4613, serial, "desk-r5.png")


def r5empty():
    disk = build_disk("empty")
    serial = os.path.join(ART, "serial-empty.log")
    plain_desktop("empty", disk, 4614, serial, "desk-empty-r5.png")


def r5term():
    serial = os.path.join(ART, "serial-r5term.log")
    if os.path.exists(serial):
        os.remove(serial)
    proc, s = boot(VPROBE_DISK, 4615, serial)
    try:
        run_gui_cmd(s, "gui r5term", 6.0)
        shoot(s, "term-title-close.png")
        print("  [r5term]", serial_tail(serial, "GUI:r5term"))
    finally:
        q.kill_qemu(proc)


def r5dbl():
    serial = os.path.join(ART, "serial-r5dbl.log")
    if os.path.exists(serial):
        os.remove(serial)
    proc, s = boot(VPROBE_DISK, 4616, serial)
    try:
        q.type_text(s, "mkdir /Desktop", 0.05); q.sendkey(s, "ret", 1.0)
        q.type_text(s, "cp /bin/hello.ce /Desktop/hello.ce", 0.05); q.sendkey(s, "ret", 1.5)
        run_gui_cmd(s, "gui r5dbl", 6.0)
        shoot(s, "desk-dblclick-ce.png")
        print("  [r5dbl]", serial_tail(serial, "GUI:r5dbl"))
        print("  [dblrun]", serial_tail(serial, "GUI:desk dbl"))
    finally:
        q.kill_qemu(proc)


def audit_browser():
    serial = os.path.join(ART, "serial-audit-browser.log")
    if os.path.exists(serial):
        os.remove(serial)
    proc, s = boot(VPROBE_DISK, 4617, serial)
    try:
        run_gui_cmd(s, "gui audit browser", 6.0)
        print("  [audit browser]", serial_tail(serial, "GUI:audit"))
    finally:
        q.kill_qemu(proc)


def term_isolate():
    serial = os.path.join(ART, "serial-term-isolate.log")
    if os.path.exists(serial):
        os.remove(serial)
    proc, s = boot(VPROBE_DISK, 4618, serial)
    try:
        run_gui_cmd(s, "gui term", 6.0)
        print("  [term isolate]", serial_tail(serial, "GUI:term"))
    finally:
        q.kill_qemu(proc)


MAIN = {
    "six": r5plain_six,
    "empty": r5empty,
    "r5desk": r5desk,
    "r5term": r5term,
    "r5dbl": r5dbl,
    "audit-browser": audit_browser,
    "term-isolate": term_isolate,
}


def main():
    which = sys.argv[1:] or list(MAIN)
    for k in which:
        print("== %s ==" % k)
        MAIN[k]()


if __name__ == "__main__":
    main()
