#!/usr/bin/env python3
"""p3_probe2.py -- follow-up P3 captures: help top, CJK fortune, sl v2."""
import os, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qemu_driver import boot, kill_qemu, type_text, sendkey, wait_for_prompt
from qemu_regress import ppm_to_png, SHOTS

PORT = 4731


def find_qemu():
    import shutil
    for c in ("/home/user/bin/qemu-system-i386", "qemu-system-i386"):
        if shutil.which(c) or os.path.isfile(c):
            return c
    return None


def cmd(s, c, wait=0.5):
    s.sendall((c + "\n").encode())
    time.sleep(wait)
    try:
        s.setblocking(False); time.sleep(0.1); s.recv(65536)
    except BlockingIOError:
        pass
    s.setblocking(True)


def shot(s, name):
    png = os.path.join(SHOTS, name + ".png")
    ppm = os.path.join(SHOTS, name + ".ppm")
    if os.path.exists(ppm): os.remove(ppm)
    cmd(s, "screendump %s" % ppm.replace("\\", "/"), 0.5)
    for _ in range(40):
        if os.path.exists(ppm) and os.path.getsize(ppm) > 0: break
        time.sleep(0.5)
    ppm_to_png(ppm, png)
    return png


def do(s, line, wait=1.2):
    type_text(s, line); sendkey(s, "ret", wait)


def main():
    iso = os.path.join(HERE, "..", "kernel_v25", "catos.iso")
    disk = os.path.join(HERE, "p3_disk.img")
    if not os.path.exists(disk):
        open(disk, "wb").truncate(64 * 1024 * 1024)
    proc, s = boot(find_qemu(), iso, PORT, disk=disk)
    if not wait_for_prompt(s, timeout=90):
        kill_qemu(proc); raise RuntimeError("no prompt")
    time.sleep(2)
    do(s, "format", 1.0); sendkey(s, "y", 0.4); sendkey(s, "ret", 10.0)

    # help: let it print, then scroll back up to capture the top section
    do(s, "help", 1.0)
    for _ in range(8):
        sendkey(s, "ctrl-up", 0.15)
    shot(s, "p3_13_help_top")

    do(s, "clear", 1.0)
    for i in range(6):
        do(s, "fortune", 0.8)
        shot(s, "p3_14_fortune_%d" % i)
        do(s, "clear", 0.6)

    do(s, "sl", 0.4); time.sleep(0.7)
    shot(s, "p3_15_sl_v2")

    do(s, "cowsay", 1.0)
    shot(s, "p3_16_cowsay_default")

    kill_qemu(proc)
    return 0


if __name__ == "__main__":
    sys.exit(main())
