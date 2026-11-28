#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""M5 第一刀：GUI 汉化取证探针（zh）。每个场景独立起 QEMU，避免应用钩子吞掉
后续 shell 指令。产出 5 张截图到 artifacts/final5/i18n/：
  i18n-menu.png          菜单 8 项中文 + 加速键
  i18n-window-titles.png 终端 + 文件管理器窗口标题中文
  i18n-statusbar.png     终端/FM 状态栏中文不溢出（记事本状态另见同批截图）
  i18n-taskbar.png       任务栏标签与窗口标题一致
  i18n-error.png         浏览器错误页中文可读
"""
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from qemu_driver import (boot, find_qemu, force_text, kill_qemu,
                         sendkey, type_text, wait_for_prompt, cmd)

SHOT_DIR = os.path.join(REPO, "artifacts", "final5", "i18n")
os.makedirs(SHOT_DIR, exist_ok=True)


def shoot(sock, name):
    ppm = os.path.join(SHOT_DIR, name + ".ppm")
    png = os.path.join(SHOT_DIR, name + ".png")
    if os.path.exists(ppm):
        os.remove(ppm)
    cmd(sock, "screendump %s" % ppm.replace("\\", "/"), 0.7)
    cmd(sock, "screendump %s" % ppm.replace("\\", "/"), 0.7)
    for _ in range(10):
        if os.path.exists(ppm) and os.path.getsize(ppm) > 0:
            break
        time.sleep(0.3)
    if os.path.exists(ppm):
        subprocess.run(["convert", ppm, png], check=False)
        os.remove(ppm)
    print("SHOT", png)
    return png


def do(s, line, wait=1.0):
    type_text(s, line)
    sendkey(s, "ret", wait)


def run_scenario(qemu, iso, disk, port, steps, shots):
    proc, s = boot(qemu, iso, port, disk=disk, extra_args=["-vga", "std"])
    try:
        force_text(s, settle=10)
        if not wait_for_prompt(s, timeout=120.0):
            print("FATAL: prompt never appeared")
            return 3
        do(s, "lang zh", 1.0)
        do(s, "clear", 0.6)
        for line, wait in steps:
            do(s, line, wait)
        time.sleep(2.5)
        for name in shots:
            shoot(s, name)
    finally:
        kill_qemu(proc)
    return 0


def main():
    iso = os.path.abspath(os.path.join(REPO, "kernel_v25", "catos.iso"))
    qemu = find_qemu()
    if not qemu:
        print("qemu-system-i386 not found")
        return 2
    disk = os.path.join(HERE, "qemu_disk_b3.img")
    if not os.path.exists(disk):
        with open(disk, "wb") as fh:
            fh.truncate(64 * 1024 * 1024)

    # 1) 菜单
    run_scenario(qemu, iso, disk, 4657,
                 [("gui shot-menu", 1.5)],
                 ["i18n-menu"])
    # 2) 终端 + 文件管理器：标题 / 状态栏 / 任务栏
    run_scenario(qemu, iso, disk, 4658,
                 [("gui mw tb", 1.5)],
                 ["i18n-window-titles", "i18n-statusbar", "i18n-taskbar"])
    # 3) 浏览器错误页（无网 -> 错误提示中文）
    run_scenario(qemu, iso, disk, 4659,
                 [("gui browser http://10.255.255.1/dead", 2.0)],
                 ["i18n-error"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
