#!/usr/bin/env python3
"""p3_probe.py -- NIGHT-P3 easter-egg + UX probe.

Boots the CatOS ISO on its own monitor port (4730), formats the scratch
disk once, then drives each new builtin through the PS/2 keyboard and
screendumps a PNG per step.  Reuses qemu_regress.ppm_to_png.

usage: python3 tests/p3_probe.py <catos.iso> [--qemu PATH]
"""
import os, sys, time, subprocess, socket

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qemu_driver import boot, kill_qemu, type_text, sendkey, wait_for_prompt
from qemu_regress import ppm_to_png, SHOTS

PORT = 4730


def find_qemu(explicit=None):
    import shutil
    if explicit:
        return explicit
    for c in ("/home/user/bin/qemu-system-i386", "qemu-system-i386"):
        if shutil.which(c) or os.path.isfile(c):
            return c
    return None


def cmd(s, c, wait=0.5):
    s.sendall((c + "\n").encode())
    time.sleep(wait)
    try:
        s.setblocking(False)
        time.sleep(0.1)
        s.recv(65536)
    except BlockingIOError:
        pass
    s.setblocking(True)


def shot(s, name):
    png = os.path.join(SHOTS, name + ".png")
    ppm = os.path.join(SHOTS, name + ".ppm")
    if os.path.exists(ppm):
        os.remove(ppm)
    cmd(s, "screendump %s" % ppm.replace("\\", "/"), 0.5)
    for _ in range(40):
        if os.path.exists(ppm) and os.path.getsize(ppm) > 0:
            break
        time.sleep(0.5)
    else:
        raise RuntimeError("no screendump for %s" % name)
    ppm_to_png(ppm, png)
    return png


def do(s, line, wait=1.2):
    type_text(s, line)
    sendkey(s, "ret", wait)


def main():
    args = sys.argv[1:]
    iso = args.pop(0) if args and not args[0].startswith("--") else None
    qemu = find_qemu()
    if "--qemu" in args:
        i = args.index("--qemu"); qemu = args.pop(i + 1); args.pop(i)
    if iso is None:
        iso = os.path.join(HERE, "..", "kernel_v25", "catos.iso")
    os.makedirs(SHOTS, exist_ok=True)

    disk = os.path.join(HERE, "p3_disk.img")
    if not os.path.exists(disk):
        with open(disk, "wb") as f:
            f.truncate(64 * 1024 * 1024)

    proc, s = boot(qemu, iso, PORT, disk=disk)
    print("monitor connected on", PORT)
    if not wait_for_prompt(s, timeout=90):
        kill_qemu(proc)
        raise RuntimeError("shell prompt never appeared")
    time.sleep(2)

    # fresh scratch disk -> format once (y + Enter, full-line confirm)
    do(s, "format", 1.0)
    sendkey(s, "y", 0.4)
    sendkey(s, "ret", 10.0)
    print("formatted")

    steps = [
        ("p3_01_cowsay",    lambda: do(s, "cowsay Hello CatOS", 1.2)),
        ("p3_02_tux",       lambda: do(s, "cowsay -f tux Hello Tux", 1.2)),
        ("p3_03_sl",        lambda: (do(s, "sl", 0.4), time.sleep(0.7))),
        ("p3_04_fortune",   lambda: do(s, "fortune", 1.2)),
        ("p3_05_cal",       lambda: do(s, "cal", 1.2)),
        ("p3_06_cal2026",   lambda: do(s, "cal 2026", 1.2)),
        ("p3_07_yes",       lambda: (do(s, "yes", 0.6),
                                     sendkey(s, "ctrl-c", 1.0))),
        ("p3_08_echo_red",  lambda: do(s, "echo -c red Red text here", 1.0)),
        ("p3_09_echo_n",    lambda: do(s, "echo -n no trailing newline", 1.0)),
        ("p3_10_cls",       lambda: do(s, "cls", 1.0)),
        ("p3_11_suggest",   lambda: do(s, "helo", 1.2)),
        ("p3_12_help",      lambda: do(s, "help", 1.5)),
    ]
    for name, fn in steps:
        try:
            fn()
            p = shot(s, name)
            print("OK  ", name, p)
        except Exception as e:
            print("ERR ", name, e)

    kill_qemu(proc)
    return 0


if __name__ == "__main__":
    sys.exit(main())
