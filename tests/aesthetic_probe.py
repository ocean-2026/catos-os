#!/usr/bin/env python3
"""aesthetic_probe.py -- on-metal screenshots for the second review
(aesthetic/terminal UI).  Boots an IDE disk, formats it once so /bin +
/pkg are seeded, then walks the terminal surface one command per shot.
Screen text is dumped alongside each PNG for traceability.

The formatted disk is cached as tests/qemu_disk_aesthetic.img, so reruns
skip the format step.  Delete that file to re-seed from scratch.

usage:
    python3 tests/aesthetic_probe.py [--round 1] [--port 4760]
"""
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qemu_driver import (find_qemu, kill_qemu, read_screen, sendkey,
                         type_text, wait_for_prompt)  # noqa: E402
from qemu_regress import ppm_to_png  # noqa: E402

REPO = os.path.dirname(HERE)
ISO = os.path.join(REPO, "kernel_v25", "catos.iso")

ROUND = "1"
PORT = 4760
_argv = sys.argv[1:]
for _i, a in enumerate(_argv):
    if a == "--round" and _i + 1 < len(_argv):
        ROUND = _argv[_i + 1]
    elif a.startswith("--round="):
        ROUND = a.split("=", 1)[1]
    elif a == "--port" and _i + 1 < len(_argv):
        PORT = int(_argv[_i + 1])
    elif a.startswith("--port="):
        PORT = int(a.split("=", 1)[1])

SHOTS = os.path.join(HERE, "qemu_shots_aesthetic")
os.makedirs(SHOTS, exist_ok=True)
AUDIT = os.path.join(HERE, "audit_logs", "aesthetic_round%s.txt" % ROUND)
os.makedirs(os.path.dirname(AUDIT), exist_ok=True)

CACHED_DISK = os.path.join(HERE, "qemu_disk_aesthetic.img")


def cmd(s, c, wait=0.5):
    s.sendall((c + "\n").encode())
    time.sleep(wait)


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
        raise RuntimeError("screendump produced no file for %s" % name)
    ppm_to_png(ppm, png)
    return png


def boot(qemu, disk, port):
    os.makedirs(tempfile.gettempdir(), exist_ok=True)
    stage = os.path.join(tempfile.gettempdir(), "catos_aesthetic_stage.img")
    shutil.copy2(disk, stage)
    cmdline = [qemu, "-m", "256", "-no-reboot", "-net", "none",
               "-cdrom", ISO, "-boot", "order=d", "-display", "none",
               "-monitor", "tcp:127.0.0.1:%d,server,nowait" % port,
               "-drive", "file=%s,format=raw,if=ide" % stage]
    proc = subprocess.Popen(cmdline, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    for _ in range(40):
        if proc.poll() is not None:
            raise RuntimeError("qemu exited rc=%r" % proc.returncode)
        try:
            return proc, socket.create_connection(("127.0.0.1", port), timeout=5), stage
        except OSError:
            time.sleep(1)
    kill_qemu(proc)
    raise RuntimeError("no monitor")


audit = open(AUDIT, "w")


def dump(s, title):
    txt = read_screen(s)
    audit.write("\n===== %s =====\n%s\n" % (title, txt))
    audit.flush()
    return txt


def run_step(s, name, command=None, wait=1.4, mid_wait=None):
    if command is not None:
        type_text(s, command)
        sendkey(s, "ret", wait)
    if mid_wait:
        time.sleep(mid_wait)
    p = shot(s, name)
    dump(s, name + "  cmd=" + (command or "(boot)"))
    print("shot: %s" % os.path.basename(p))


def main():
    qemu = find_qemu()
    if not qemu:
        qemu = "/home/user/bin/qemu-system-i386"

    fresh = not os.path.exists(CACHED_DISK)
    if fresh:
        with open(CACHED_DISK, "wb") as f:
            f.truncate(64 * 1024 * 1024)
    proc, s, stage = boot(qemu, CACHED_DISK, PORT)
    try:
        time.sleep(25)
        if not wait_for_prompt(s, 30):
            raise RuntimeError("shell prompt never appeared")
        run_step(s, "00_boot", None, wait=0)

        if fresh:
            type_text(s, "format")
            sendkey(s, "ret", 1.5)
            sendkey(s, "y", 0.3)
            sendkey(s, "ret", 15.0)
            time.sleep(3.0)
            dump(s, "01_format")
            wait_for_prompt(s, 20)
            shutil.copy2(stage, CACHED_DISK)

        steps = [
            ("02_about",      "about",            1.4),
            ("03_ls_root",    "ls /",             1.2),
            ("04_ls_bin",     "ls /bin",          1.4),
            ("05_ls_long",    "ls -l /bin",       1.4),
            ("06_help",       "help",             1.6),
            ("07_df",         "df",               1.2),
            ("08_mount",      "mount",            1.2),
            ("09_pkg_list",   "pkg list",         1.4),
            ("10_pkg_info",   "pkg info bc",       1.2),
            ("11_tree",       "tree /",           1.6),
            ("12_cowsay",     "cowsay Hello, CatOS!", 1.2),
            ("13_cal",        "cal",              1.2),
            ("14_fortune",    "fortune",          1.2),
            ("15_unknown",    "helo",              1.2),
            ("16_echo_red",   "echo -c red Red sample text", 1.0),
            ("17_echo_green", "echo -c green Green sample", 1.0),
            ("18_echo_blue",  "echo -c blue Blue sample", 1.0),
            ("19_prompt_cd",  "cd /bin",           1.0),
        ]
        for name, c, w in steps:
            run_step(s, name, c, wait=w)

        # sl animation: capture a mid frame
        type_text(s, "sl")
        sendkey(s, "ret", 0.5)
        run_step(s, "20_sl", None, wait=0, mid_wait=0.3)

        # gfx (256-color styled text) mode frame, then back to text
        type_text(s, "gfx")
        sendkey(s, "ret", 1.6)
        run_step(s, "21_gfx", None)
        type_text(s, "text")
        sendkey(s, "ret", 1.0)
        sendkey(s, "y", 1.2)
        dump(s, "22_text_back")
        shutil.copy2(stage, CACHED_DISK)
    finally:
        audit.close()
        kill_qemu(proc)
        if os.path.exists(stage):
            os.remove(stage)


if __name__ == "__main__":
    main()
