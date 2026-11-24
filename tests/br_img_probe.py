#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""br_img_probe.py -- 浏览器 <img> BMP/PPM 同源解码验收。

挂载 gui-samples.img（内含 /samples/desktop.bmp、desktop.ppm），guest 打开
内嵌样本页（引用 /samples/desktop.bmp），串口回打 ``GUI:br img <src> w=.. h=..``。
断言：BMP 解码成功并贴图、尺寸与文件一致；截图落 artifacts。

usage:
    python3 tests/br_img_probe.py [catos.iso]
"""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qemu_driver import (find_qemu, boot, kill_qemu, screen_text,  # noqa: E402
                         type_text, sendkey, wait_for_prompt, _stage_image,
                         force_text)

REPO = os.path.dirname(HERE)
MONITOR_PORT = int(os.environ.get("BR_IMG_MON", "4571"))
SAMPLES_IMG = os.environ.get(
    "BR_IMG_DISK",
    "/home/user/work/artifacts/gui-blocks/samples/gui-samples.img")

ART = os.path.join(REPO, "artifacts", "overnight-20261124")
os.makedirs(ART, exist_ok=True)
SERIAL = os.path.join(ART, "br-img-serial.log")
SHOT_PPM = os.path.join(ART, "shots", "browser-bmp.ppm")
SHOT_PNG = os.path.join(ART, "shots", "browser-bmp.png")


def main():
    iso = os.path.abspath(sys.argv[1] if len(sys.argv) > 1
                          else os.path.join(REPO, "kernel_v25", "catos.iso"))
    qemu = find_qemu(None)
    if not qemu:
        print("FAIL: qemu-system-i386 not found")
        return 2

    if os.path.exists(SERIAL):
        os.remove(SERIAL)
    for p in (SHOT_PPM, SHOT_PNG):
        if os.path.exists(p):
            os.remove(p)

    disk = SAMPLES_IMG if os.path.exists(SAMPLES_IMG) else None
    proc = sock = None
    results = []

    def step(name, ok, note=""):
        results.append((name, bool(ok)))
        print("%-4s %-40s %s" % ("PASS" if ok else "FAIL", name, note))

    try:
        proc, sock = boot(qemu, iso, MONITOR_PORT, disk=disk,
                          extra_args=["-serial", "file:" + SERIAL])
        force_text(sock)
        if not wait_for_prompt(sock, timeout=120.0):
            print("FATAL: shell prompt never appeared")
            return 3

        type_text(sock, "gui browser sample://local")
        sendkey(sock, "ret", 1.0)
        time.sleep(9.0)
        screen_text(sock)

        try:
            text = open(SERIAL, "rb").read().decode("utf-8", "replace")
        except OSError:
            text = ""

        import re
        m = re.search(r"GUI:br img (\S+) w=(\d+) h=(\d+)", text)
        step("BMP img decoded and embedded", m is not None,
             m.group(0) if m else "no GUI:br img marker")
        if m:
            src, w, h = m.group(1), int(m.group(2)), int(m.group(3))
            step("BMP geometry 800x600", w == 800 and h == 600,
                 "src=%s %dx%d" % (src, w, h))

        # 截图留证。
        from qemu_driver import cmd
        os.makedirs(os.path.dirname(SHOT_PPM), exist_ok=True)
        cmd(sock, "screendump %s" % SHOT_PPM, wait=1.5)
        time.sleep(1.0)
        try:
            from PIL import Image
            Image.open(SHOT_PPM).convert("RGB").save(SHOT_PNG)
            print("  evidence: %s" % SHOT_PNG)
        except Exception as e:
            print("  screenshot conversion failed: %s" % e)
    finally:
        kill_qemu(proc)

    npass = sum(1 for _, ok in results if ok)
    print("========== SUMMARY ==========")
    for name, ok in results:
        print("PASS" if ok else "FAIL", name)
    print("%d/%d passed" % (npass, len(results)))
    return 0 if npass == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
