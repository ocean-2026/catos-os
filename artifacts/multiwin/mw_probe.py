#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""多窗口隔离取证：逐模式起 QEMU，发 `gui mw <mode>`，抓串口打点 + screendump。"""
import os, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, "tests"))
import qemu_driver as q

ISO = os.path.join(REPO, "kernel_v25", "catos.iso")
DISK = os.path.join(REPO, "tests", "qemu_disk_vprobe.img")
ART = HERE

MODES = [
    ("term",  "two-term-isolated.png"),
    ("fm",    "two-fm-diff-dir.png"),
    ("focus", "focus-switch.png"),
    ("cap",   "four-term-cap.png"),
    ("close", "close-one-keeps-other.png"),
]

def main():
    only = sys.argv[1:] or [m for m, _ in MODES]
    for mode, png in MODES:
        if mode not in only:
            continue
        serial = os.path.join(ART, "serial-mw-%s.log" % mode)
        if os.path.exists(serial):
            os.remove(serial)
        qemu = q.find_qemu()
        proc, s = q.boot(qemu, ISO, 4630 + MODES.index((mode, png)), disk=DISK,
                         extra_args=["-smp", "1", "-cpu", "qemu32", "-vga", "std",
                                     "-serial", "file:" + serial])
        try:
            q.force_text(s, settle=10)
            if not q.wait_for_prompt(s, timeout=90):
                print("FAIL: no prompt", mode); continue
            q.type_text(s, "gui mw " + mode, 0.05)
            q.sendkey(s, "ret", 1.0)
            time.sleep(6.0)
            txt = open(serial, encoding="utf-8", errors="replace").read()
            for ln in txt.splitlines():
                if any(k in ln for k in ("MW:", "TERM:", "FM:")):
                    print("[%s] %s" % (mode, ln))
            ppm = os.path.join(ART, png.replace(".png", ".ppm"))
            if os.path.exists(ppm): os.remove(ppm)
            q.cmd(s, "screendump " + ppm, 1.0)
            deadline = time.time() + 10
            while time.time() < deadline:
                if os.path.exists(ppm) and os.path.getsize(ppm) > 0: break
                time.sleep(0.3)
            try:
                from PIL import Image
                Image.open(ppm).convert("RGB").save(os.path.join(ART, png))
                print("SHOT", png)
            except Exception as e:
                print("PNG convert fail", e)
        finally:
            q.kill_qemu(proc)
            time.sleep(1)

if __name__ == "__main__":
    main()
