#!/usr/bin/env python3
"""gui_pixel_regress.py -- M0 桌面像素断言回归（配合 gui_pixel_assert.py）。

流程：固定 `-vga std` 起客人机 -> 进文本控制台 -> 输入 `gui` 进块桌面
->  settle 后 screendump 落 PPM（同时转 PNG 留存）-> 调用
gui_pixel_assert 的默认三条断言。

用法：
    python3 tests/gui_pixel_regress.py [iso] [--cmd gui] [--settle 6]
    python3 tests/gui_pixel_regress.py --ppm <frame.ppm>   # 只验已有 PPM

环境：
    PROBES_DIR  外部探针基建目录（默认 /home/user/work/artifacts/gui-blocks/probes）
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

from qemu_driver import (find_qemu, boot, kill_qemu, force_text,  # noqa: E402
                         wait_for_prompt, type_text, sendkey, cmd)
import gui_pixel_assert as pa  # noqa: E402

ART = os.path.join(REPO, "artifacts", "gui-blocks")
os.makedirs(ART, exist_ok=True)
ISO = os.path.join(REPO, "kernel_v25", "catos.iso")
PORT = 4481


def convert_png(ppm, png):
    try:
        from PIL import Image
        Image.open(ppm).convert("RGB").save(png)
        return True
    except Exception:
        conv = os.path.join(PROBES_DIR, "ppm2png.py")
        if os.path.exists(conv):
            r = subprocess.run([sys.executable, conv, ppm, png])
            return r.returncode == 0
    return False


def capture(iso, shell_cmd, settle, ppm, png, port=PORT):
    qemu = find_qemu()
    if not qemu:
        print("FAIL: qemu-system-i386 not found")
        return 2
    serial = os.path.join(ART, "pixel-serial.log")
    if os.path.exists(serial):
        os.remove(serial)
    scratch = os.environ.get(
        "SCRATCH_IMG",
        "/home/user/work/artifacts/gui-blocks/samples/gui-samples.img")
    disk = scratch if os.path.exists(scratch) else None
    # 与可出帧的 gui_runner/pixprobe 对齐：vmware 显式 VGA 才把 VBE24
    # LFB 暴露给 monitor screendump；默认 cirrus 下抓不到 PPM。
    proc, sock = boot(qemu, iso, port, disk=disk,
                      extra_args=["-smp", "1", "-cpu", "qemu32",
                                  "-vga", "vmware",
                                  "-serial", "file:" + serial])
    try:
        force_text(sock)
        if not wait_for_prompt(sock, timeout=90.0):
            print("FAIL: shell prompt never appeared")
            return 3
        type_text(sock, shell_cmd)
        sendkey(sock, "ret", 1.0)
        time.sleep(settle)
        if os.path.exists(ppm):
            os.remove(ppm)
        hmp = cmd(sock, "screendump %s" % ppm, 1.0)
        if hmp.strip():
            print("hmp:", hmp.strip()[:200])
        time.sleep(2.0)
        deadline = time.time() + 10.0
        while time.time() < deadline:
            if os.path.exists(ppm) and os.path.getsize(ppm) > 0:
                break
            time.sleep(0.3)
        if not (os.path.exists(ppm) and os.path.getsize(ppm) > 0):
            print("FAIL: screendump produced no PPM")
            return 4
        if png:
            convert_png(ppm, png)
        return 0
    finally:
        kill_qemu(proc)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("iso", nargs="?", default=ISO)
    ap.add_argument("--cmd", default=None,
                    help="shell command that enters the frame; defaults to "
                         "the shot subcommand matching --group")
    ap.add_argument("--settle", type=float, default=6.0)
    ap.add_argument("--ppm", default=None, help="verify an existing PPM only")
    ap.add_argument("--png", default=os.path.join(ART, "m0-desktop.png"))
    ap.add_argument("--port", type=int, default=PORT)
    ap.add_argument("--group", choices=sorted(pa.CHECK_GROUPS.keys()),
                    default="dark", help="pixel assert group")
    args = ap.parse_args()

    GROUP_CMD = {
        "dark":  "gui",
        "light": "gui shot-light",
        "hc":    "gui shot-hc",
        "menu":  "gui shot-menu",
        "drag":  "gui shot-drag",
        "crop":  "gui shot-crop",
        "notepad": "gui shot-notepad",
        "canvas":  "gui shot-canvas",
        "files":   "gui shot-files",
        "images":  "gui shot-images",
        "browser": "gui shot-browser",
        "style-app":  "gui shot-style-app",
        "style-calm": "gui shot-style-calm",
    }
    cmd = args.cmd or GROUP_CMD[args.group]

    ppm = args.ppm or os.path.join(ART, "m0-desktop.ppm")
    if not args.ppm:
        rc = capture(args.iso, cmd, args.settle, ppm, args.png,
                      args.port)
        if rc != 0:
            return rc

    try:
        ppm_obj = pa.Ppm(ppm)
    except (OSError, RuntimeError) as e:
        print("FAIL: %s" % e)
        return 2

    checks = pa.CHECK_GROUPS[args.group]
    bg = pa.GROUP_BG[args.group]
    print("frame=%dx%d group=%s bg=%s" % (ppm_obj.w, ppm_obj.h, args.group, bg))
    passed, failures = pa._run_checks(ppm_obj, checks, bg, 6)
    print("== %d/%d checks passed ==" % (passed, len(checks)))
    if failures:
        print("RESULT=FAIL")
        for f in failures:
            print("  - " + f)
        return 1
    print("RESULT=PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
