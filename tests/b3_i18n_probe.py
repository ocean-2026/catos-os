#!/usr/bin/env python3
"""b3_i18n_probe.py -- B3: batch-trigger bilingualized success prompts in zh.

Boots the freshly built ISO, switches to zh locale, fires a batch of
shell commands whose success path was wrapped in trs(), then dumps the
screen text and a PNG screenshot as on-metal evidence.
"""
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qemu_driver import (boot, find_qemu, force_text, kill_qemu,  # noqa: E402
                         screen_text, sendkey, type_text, wait_for_prompt, cmd)

REPO = os.path.dirname(HERE)
MONITOR_PORT = 4463
SHOT_DIR = os.path.join(REPO, "artifacts", "b3-i18n")


def shoot(sock, name):
    os.makedirs(SHOT_DIR, exist_ok=True)
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
    return png


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

    proc, s = boot(qemu, iso, MONITOR_PORT, disk=disk)
    try:
        force_text(s)
        if not wait_for_prompt(s, timeout=120.0):
            print("FATAL: prompt never appeared")
            return 3

        def do(line, wait=1.0):
            type_text(s, line)
            sendkey(s, "ret", wait)

        # fresh disk: format once so /etc persistence paths succeed
        type_text(s, "format")
        sendkey(s, "ret", 1.0)
        sendkey(s, "y", 0.4)
        sendkey(s, "ret", 15.0)
        wait_for_prompt(s, timeout=90.0)

        do("lang zh", 1.0)
        do("clear", 0.6)

        batch = [
            "style fg #ff8888",
            "style bg #222222",
            "theme light",
            "clip -c",
            "autostart --install gui",
            "fsck",
            "passwd root catos123",
            "useradd b3user 1020 1020 0",
            "groupadd b3grp",
        ]
        for line in batch:
            do(line, 1.2)

        time.sleep(1.0)
        text = screen_text(s)
        os.makedirs(SHOT_DIR, exist_ok=True)
        with open(os.path.join(SHOT_DIR, "b3-screen.txt"), "w", encoding="utf-8") as fh:
            fh.write(text)
        print("---- screen ----")
        print(text)
        png = shoot(s, "b3-zh-success")
        print("shot:", png)
    finally:
        kill_qemu(proc)
    return 0


if __name__ == "__main__":
    sys.exit(main())
