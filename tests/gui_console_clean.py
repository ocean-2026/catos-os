#!/usr/bin/env python3
"""gui_console_clean.py -- `gui` 进出后文本控制台洁净度回归。

流程：固定 `-vga std` 起机 -> 进文本控制台 -> 抓「进入前」文本帧 ->
输入 `gui` 进块桌面 -> Esc 退回 -> 抓「退出后」文本帧 -> 两帧逐点比对。
退出路径应走 con_force_text + con_clear，退出后文本平面只剩提示符，
不残留桌面图形碎片。

用法：
    python3 tests/gui_console_clean.py [iso] [--port 4576] [--settle 6]

退出码：0 洁净；1 差异超阈值；2 输入/驱动错误。
"""
import argparse
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)

PROBES_DIR = os.environ.get(
    "PROBES_DIR", "/home/user/work/artifacts/gui-blocks/probes")
sys.path.insert(0, os.path.join(PROBES_DIR, "gates"))
sys.path.insert(0, PROBES_DIR)

from qemu_driver import (boot, find_qemu, force_text, kill_qemu,  # noqa: E402
                         wait_for_prompt, type_text, sendkey, cmd)
import gui_pixel_assert as pa  # noqa: E402

ART = os.path.join(REPO, "artifacts", "gui-blocks")
os.makedirs(ART, exist_ok=True)
ISO = os.path.join(REPO, "kernel_v25", "catos.iso")

# 文本帧允许的差异像素占比上限（提示符重绘/光标闪烁可忽略）。
MAX_DIFF_RATIO = 0.05


def dump(sock, ppm, settle=1.0):
    if os.path.exists(ppm):
        os.remove(ppm)
    cmd(sock, "screendump %s" % ppm, wait=settle)
    deadline = time.time() + 10.0
    while time.time() < deadline:
        if os.path.exists(ppm) and os.path.getsize(ppm) > 0:
            break
        time.sleep(0.3)
    return os.path.exists(ppm) and os.path.getsize(ppm) > 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("iso", nargs="?", default=ISO)
    ap.add_argument("--port", type=int, default=4576)
    ap.add_argument("--settle", type=float, default=6.0)
    ap.add_argument("--tol", type=int, default=6)
    args = ap.parse_args()

    qemu = find_qemu()
    if not qemu:
        print("FAIL: qemu-system-i386 not found")
        return 2

    before = os.path.join(ART, "console-before.ppm")
    after = os.path.join(ART, "console-after.ppm")
    serial = os.path.join(ART, "console-clean-serial.log")

    proc, sock = boot(qemu, args.iso, args.port,
                      extra_args=["-vga", "std", "-serial", "file:" + serial])
    try:
        force_text(sock)
        if not wait_for_prompt(sock, timeout=90.0):
            print("FAIL: shell prompt never appeared")
            return 3
        if not dump(sock, before):
            print("FAIL: before screendump empty")
            return 4

        type_text(sock, "gui")
        sendkey(sock, "ret", 1.0)
        time.sleep(args.settle)

        sendkey(sock, "esc", 1.0)
        time.sleep(2.0)
        if not wait_for_prompt(sock, timeout=30.0):
            print("FAIL: did not return to text console after Esc")
            return 5
        if not dump(sock, after):
            print("FAIL: after screendump empty")
            return 4

        a = pa.Ppm(before)
        b = pa.Ppm(after)
        if (a.w, a.h) != (b.w, b.h):
            print("FAIL: size mismatch before=%dx%d after=%dx%d" %
                  (a.w, a.h, b.w, b.h))
            return 2

        diff = 0
        total = a.w * a.h
        for y in range(a.h):
            for x in range(a.w):
                pa_px = a.px(x, y)
                pb_px = b.px(x, y)
                if not pa._close(pa_px, pb_px, args.tol):
                    diff += 1
        ratio = diff / total
        print("frame=%dx%d diff_px=%d/%d ratio=%.4f tol=%d" %
              (a.w, a.h, diff, total, ratio, args.tol))
        if ratio > MAX_DIFF_RATIO:
            print("RESULT=FAIL: console residual above %.0f%%" %
                  (MAX_DIFF_RATIO * 100))
            return 1
        print("RESULT=PASS: console clean after gui enter/exit")
        return 0
    finally:
        kill_qemu(proc)


if __name__ == "__main__":
    sys.exit(main())
