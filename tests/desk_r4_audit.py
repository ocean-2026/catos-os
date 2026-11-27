#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""R4 七组件裁剪/塌陷审计。

Pass 1 几何：`gui audit <comp>` 只读遍历块表，串口回 `GUI:audit comp=.. zero=.. overflow=..`。
Pass 2 截图：`gui shot-<comp>` 进组件态 idle，screendump 存 clip-audit/。

用法: python3 tests/desk_r4_audit.py [comp ...]
"""
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from qemu_driver import (boot, find_qemu, kill_qemu, force_text,
                         wait_for_prompt, sendkey, type_text, cmd)

ISO = os.path.join(REPO, "kernel_v25", "catos.iso")
ART = os.path.join(REPO, "artifacts", "desktop-icons")
CLIP = os.path.join(ART, "clip-audit")
os.makedirs(CLIP, exist_ok=True)

COMPS = ["notepad", "canvas", "files", "images", "browser", "settings", "term"]
SHOT = {
    "notepad": "shot-notepad",
    "canvas": "shot-canvas",
    "files": "shot-files",
    "images": "shot-images",
    "browser": "shot-browser",
    "settings": "shot-settings",
    "term": None,
}


def read_serial(path):
    if not os.path.exists(path):
        return ""
    return open(path, "rb").read().decode("latin-1", "replace")


def shoot(proc_s, ppm, png):
    from PIL import Image
    if os.path.exists(ppm):
        os.remove(ppm)
    cmd(proc_s, "screendump " + ppm, 1.0)
    deadline = time.time() + 10
    while time.time() < deadline:
        if os.path.exists(ppm) and os.path.getsize(ppm) > 0:
            break
        time.sleep(0.3)
    Image.open(ppm).convert("RGB").save(png)
    print("  SHOT", png)


def run_audit(comp, port):
    serial = os.path.join(CLIP, "audit-%s.log" % comp)
    if os.path.exists(serial):
        os.remove(serial)
    qemu = find_qemu(None)
    proc, s = boot(qemu, ISO, port,
                    extra_args=["-smp", "1", "-cpu", "qemu32", "-vga", "vmware",
                                "-serial", "file:" + serial])
    try:
        force_text(s, settle=10)
        if not wait_for_prompt(s, timeout=90):
            print("  FAIL prompt", comp)
            return None
        type_text(s, "gui audit " + comp, 0.05)
        sendkey(s, "ret", 1.0)
        time.sleep(6.0)
        txt = read_serial(serial)
        m = re.search(r"GUI:audit comp=(\S+) zero=(\d+) overflow=(\d+)", txt)
        zeros = re.findall(r"GUI:audit-zero .*", txt)
        overs = re.findall(r"GUI:audit-over .*", txt)
        if m:
            print("  [%s] comp=%s zero=%s overflow=%s" %
                  (comp, m.group(1), m.group(2), m.group(3)))
        else:
            print("  [%s] audit line NOT FOUND" % comp)
        for z in zeros:
            print("    ZERO", z)
        for o in overs:
            print("    OVER", o)
        return m
    finally:
        kill_qemu(proc)


def run_shot(comp, port):
    if SHOT[comp] is None:
        return None
    serial = os.path.join(CLIP, "shot-%s.log" % comp)
    qemu = find_qemu(None)
    proc, s = boot(qemu, ISO, port,
                    extra_args=["-smp", "1", "-cpu", "qemu32", "-vga", "vmware",
                                "-serial", "file:" + serial])
    try:
        force_text(s, settle=10)
        if not wait_for_prompt(s, timeout=90):
            print("  FAIL prompt", comp)
            return
        type_text(s, "gui " + SHOT[comp], 0.05)
        sendkey(s, "ret", 1.0)
        time.sleep(6.0)
        shoot(s, os.path.join(CLIP, comp + ".ppm"),
              os.path.join(CLIP, comp + ".png"))
        sendkey(s, "esc", 1.0)
        time.sleep(1.0)
    finally:
        kill_qemu(proc)


def main():
    which = sys.argv[1:] or COMPS
    for i, comp in enumerate(which):
        print("== audit", comp, "==")
        run_audit(comp, 4600 + i)
        print("== shot", comp, "==")
        run_shot(comp, 4610 + i)


if __name__ == "__main__":
    main()
