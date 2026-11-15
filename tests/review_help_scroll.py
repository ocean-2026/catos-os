#!/usr/bin/env python3
"""review_help_scroll.py -- deep scrollback capture of the help top rows.

The fun section (cowsay/sl/fortune/cal/yes) prints near the top of `help`;
the 25-row live grid and the shallow 14-step scroll in review_round2.py
never reach it.  Walk the full archive (up to 96 rows) with Ctrl+Up and
accumulate every painted frame, then assert the P3 egg commands are there.
"""
import os
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qemu_driver import (find_qemu, kill_qemu, screen_text, sendkey,
                         type_text, wait_for_prompt)  # noqa: E402
from qemu_regress import ppm_to_png  # noqa: E402

REPO = os.path.dirname(HERE)
ISO = os.path.join(REPO, "kernel_v25", "catos.iso")
BASE_DISK = os.path.join(HERE, "qemu_disk.img")
STAGE = os.path.join(tempfile.gettempdir(), "catos_help_stage")
SHOTS = os.path.join(HERE, "qemu_shots_review")
os.makedirs(SHOTS, exist_ok=True)
REVIEW_DISK = os.path.join(STAGE, "review_disk.img")

fails = []


def check(name, ok, note=""):
    print("  %-4s %-34s %s" % ("PASS" if ok else "FAIL", name, note[:150]))
    if not ok:
        fails.append(name)


def main():
    qemu = find_qemu(None) or "/home/user/bin/qemu-system-i386"
    os.makedirs(STAGE, exist_ok=True)
    shutil.copy2(BASE_DISK, REVIEW_DISK)
    import socket
    cmd = [qemu, "-m", "256", "-no-reboot", "-net", "none", "-cdrom", ISO,
           "-boot", "order=d", "-display", "none",
           "-monitor", "tcp:127.0.0.1:4775,server,nowait",
           "-drive", "file=%s,format=raw,if=ide" % REVIEW_DISK]
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL)
    try:
        s = None
        for _ in range(40):
            if proc.poll() is not None:
                raise RuntimeError("qemu exited rc=%r" % proc.returncode)
            try:
                s = socket.create_connection(("127.0.0.1", 4775), timeout=5)
                break
            except OSError:
                time.sleep(1)
        if s is None:
            raise RuntimeError("no monitor")
        if not wait_for_prompt(s):
            check("boot prompt", False); return 1

        type_text(s, "clear"); sendkey(s, "ret", 0.8)
        type_text(s, "help"); sendkey(s, "ret", 1.0)

        archive = screen_text(s)
        for _ in range(70):
            sendkey(s, "ctrl-up", 0.18)
            time.sleep(0.12)
            archive += " " + screen_text(s)

        for tok in ("cowsay", "sl", "fortune", "cal", "yes",
                    "pkg", "head", "tail", "wc", "tree"):
            check("help archive: %s" % tok, tok in archive, "")

        ppm = os.path.join(SHOTS, "R2_help_deep.ppm")
        png = os.path.join(SHOTS, "R2_help_deep.png")
        s.sendall(("screendump %s\n" % ppm).encode())
        time.sleep(0.5)
        for _ in range(40):
            if os.path.exists(ppm) and os.path.getsize(ppm) > 0:
                break
            time.sleep(0.5)
        ppm_to_png(ppm, png)
    finally:
        kill_qemu(proc)
        shutil.rmtree(STAGE, ignore_errors=True)

    print("\n===== help scroll: %d fail =====" % len(fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
