#!/usr/bin/env python3
"""p3_probe3.py -- capture help top section via scrollback (PgUp)."""
import os, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qemu_driver import boot, kill_qemu, type_text, sendkey, wait_for_prompt
from qemu_regress import ppm_to_png, SHOTS

PORT = 4732


def find_qemu():
    import shutil
    for c in ("/home/user/bin/qemu-system-i386", "qemu-system-i386"):
        if shutil.which(c) or os.path.isfile(c):
            return c
    return None


def cmd(s, c, wait=0.5):
    s.sendall((c + "\n").encode()); time.sleep(wait)
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


def main():
    iso = os.path.join(HERE, "..", "kernel_v25", "catos.iso")
    disk = os.path.join(HERE, "p3_disk.img")
    if not os.path.exists(disk):
        open(disk, "wb").truncate(64 * 1024 * 1024)
    proc, s = boot(find_qemu(), iso, PORT, disk=disk)
    if not wait_for_prompt(s, timeout=90):
        kill_qemu(proc); raise RuntimeError("no prompt")
    time.sleep(2)
    type_text(s, "format"); sendkey(s, "ret", 1.0)
    sendkey(s, "y", 0.4); sendkey(s, "ret", 10.0)

    type_text(s, "help"); sendkey(s, "ret", 1.0)
    for _ in range(40):
        sendkey(s, "ctrl-up", 0.08)
    shot(s, "p3_13_help_top")
    kill_qemu(proc)
    return 0


if __name__ == "__main__":
    sys.exit(main())
