#!/usr/bin/env python3
"""Shared helpers for the F11 (passwd digest hardening) on-metal probes.

Boots a QEMU guest off the freshly built ISO with a COPIED test disk, drives
the shell over the HMP monitor, and collects screen text + screendump PPMs.
"""
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, "tests"))
from qemu_driver import (find_qemu, kill_qemu, read_screen, screen_text,
                         sendkey, type_text, wait_for_prompt, force_text,
                         cmd)

ISO = os.path.join(REPO, "kernel_v25", "catos.iso")
BASE_DISK = os.path.join(REPO, "test_disk.img")
SHOTS = os.path.join(HERE, "shots")
os.makedirs(SHOTS, exist_ok=True)


def stage_disk(tag):
    """Copy the base disk into a private scratch file for this scenario."""
    stage = os.path.join(tempfile.gettempdir(), "catos_f11")
    os.makedirs(stage, exist_ok=True)
    dst = os.path.join(stage, "f11_%s.img" % tag)
    shutil.copy2(BASE_DISK, dst)
    return dst


def boot_guest(iso, disk, port, log=None):
    qemu = find_qemu(None)
    if not qemu:
        raise SystemExit("qemu-system-i386 not found")
    cmdline = [qemu, "-m", "256", "-no-reboot", "-net", "none",
               "-cdrom", iso, "-boot", "order=d", "-display", "none",
               "-monitor", "tcp:127.0.0.1:%d,server,nowait" % port,
               "-drive", "file=%s,format=raw,if=ide" % disk]
    if log:
        cmdline += ["-D", log]
    proc = subprocess.Popen(cmdline, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
    sock = None
    for _ in range(40):
        if proc.poll() is not None:
            raise RuntimeError("qemu exited rc=%r" % proc.returncode)
        try:
            sock = socket.create_connection(("127.0.0.1", port), timeout=5)
            break
        except OSError:
            time.sleep(2)
    if sock is None:
        kill_qemu(proc)
        raise RuntimeError("no monitor on port %d" % port)
    return proc, sock


def settle(sock, switch_text=True):
    if switch_text:
        force_text(sock)
    if not wait_for_prompt(sock, timeout=120.0):
        raise RuntimeError("shell prompt never appeared")


def run(sock, line, wait=1.4):
    import re
    type_text(sock, line)
    sendkey(sock, "ret")
    pat = re.compile(r"catos:[^\n]*[\$#]")
    deadline = time.time() + max(wait, 2.0) + 6.0
    txt = ""
    while time.time() < deadline:
        time.sleep(0.4)
        txt = read_screen(sock)
        if pat.search(txt):
            break
    time.sleep(0.3)
    txt = read_screen(sock)
    if not txt.strip():                       # mid-redraw race: retry once
        time.sleep(1.0)
        txt = read_screen(sock)
    return screen_text(sock)


def clear(sock):
    run(sock, "clear", 0.7)


def dump_text(sock, name):
    txt = read_screen(sock)
    with open(os.path.join(HERE, name + ".txt"), "w") as fh:
        fh.write(txt)
    return txt


def shot(sock, name):
    """HMP screendump -> PPM, then convert to PNG next to the PPM."""
    ppm = os.path.join(SHOTS, name + ".ppm")
    png = os.path.join(SHOTS, name + ".png")
    cmd(sock, "screendump %s" % ppm, wait=1.5)
    time.sleep(1.0)
    try:
        subprocess.run(["convert", ppm, "-resize", "200%", png],
                       check=True, capture_output=True)
    except Exception:
        # ImageMagick missing: keep the PPM as the raw evidence.
        pass
    return png if os.path.isfile(png) else ppm
