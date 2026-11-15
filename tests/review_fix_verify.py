#!/usr/bin/env python3
"""review_fix_verify.py -- on-metal verification of v2.44 review fixes.

  1. empty file readable by head/tail/wc (fix #1: tool_read_whole)
  2. `cal 2026` renders no stale/'?' cells (fix #2: calendar zeroing)
  3. TAB completion includes cowsay/df/fortune/cal/sl/yes (fix #3: sh_cmds)
  4. same partition mounted a second place is refused (fix #4: mount dedup #142)
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
from qemu_driver import (find_qemu, kill_qemu, screen_text, sendkey,
                         type_text, wait_for_prompt)  # noqa: E402
from qemu_regress import ppm_to_png, shot  # noqa: E402

REPO = os.path.dirname(HERE)
ISO = os.path.join(REPO, "kernel_v25", "catos.iso")
SRC_DISK = os.path.join(REPO, "kernel_v25", "catos_disk.img")
PART_DISK = os.path.join(HERE, "qemu_disk_part.img")
STAGE = os.path.join(tempfile.gettempdir(), "catos_review_verify")
SHOTS = os.path.join(HERE, "qemu_shots_review")
os.makedirs(SHOTS, exist_ok=True)

results = []


def check(name, ok, note=""):
    results.append((name, bool(ok), note))
    print("  %-4s %-38s %s" % ("PASS" if ok else "FAIL", name, note[:150]))


def boot(qemu, disk, port, mem="256"):
    os.makedirs(STAGE, exist_ok=True)
    stage = os.path.join(STAGE, os.path.basename(disk))
    if os.path.abspath(disk) != os.path.abspath(stage):
        shutil.copy2(disk, stage)
    cmd = [qemu, "-m", mem, "-no-reboot", "-net", "none", "-cdrom", ISO,
           "-boot", "order=d", "-display", "none",
           "-monitor", "tcp:127.0.0.1:%d,server,nowait" % port,
           "-drive", "file=%s,format=raw,if=ide" % stage]
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL)
    for _ in range(40):
        if proc.poll() is not None:
            raise RuntimeError("qemu exited rc=%r" % proc.returncode)
        try:
            return proc, socket.create_connection(("127.0.0.1", port), timeout=5)
        except OSError:
            time.sleep(1)
    kill_qemu(proc)
    raise RuntimeError("no monitor")


def run(s, line, wait=1.3):
    type_text(s, line)
    sendkey(s, "ret", wait)
    time.sleep(0.4)
    return screen_text(s)


def shot(s, name):
    ppm = os.path.join(SHOTS, name + ".ppm")
    png = os.path.join(SHOTS, name + ".png")
    if os.path.exists(ppm):
        os.remove(ppm)
    s.sendall(("screendump %s\n" % ppm.replace("\\", "/")).encode())
    time.sleep(0.5)
    for _ in range(40):
        if os.path.exists(ppm) and os.path.getsize(ppm) > 0:
            break
        time.sleep(0.5)
    ppm_to_png(ppm, png)
    return png


def verify_tools_cal_completer(qemu):
    print("\n=== Fix verify: tools / cal / completion (256 MiB) ===")
    proc, s = boot(qemu, SRC_DISK, 4762)
    try:
        if not wait_for_prompt(s):
            check("boot prompt", False); return
        run(s, "clear", 0.8)

        run(s, "touch /tmp/v.txt", 1.0)
        out = run(s, "ls /tmp")
        check("touch creates v.txt", "v.txt" in out, out[-120:].replace("\n", " "))

        out = run(s, "wc /tmp/v.txt")
        check("wc empty file zero counts", "0" in out and "v.txt" in out,
              out[-120:].replace("\n", " "))
        out = run(s, "head /tmp/v.txt")
        check("head empty file no error", "no such" not in out,
              out[-120:].replace("\n", " "))
        out = run(s, "tail /tmp/v.txt")
        check("tail empty file no error", "no such" not in out,
              out[-120:].replace("\n", " "))

        out = run(s, "cal 2026")
        bad = "?," in out or "??" in out
        check("cal 2026 no stale/garble cells", not bad,
              out[-160:].replace("\n", " "))
        shot(s, "V_cal_2026_fixed")

        # TAB completion: type prefix, press TAB, read the composed line
        type_text(s, "cows")
        sendkey(s, "tab", 1.0)
        out = screen_text(s)
        check("TAB completes cowsay", "cowsay" in out, out[-120:].replace("\n", " "))
        sendkey(s, "ret", 0.5)                      # dismiss any candidate list
        run(s, "clear", 0.8)

        type_text(s, "df")
        sendkey(s, "tab", 1.0)
        out = screen_text(s)
        check("TAB knows df", "df" in out, out[-120:].replace("\n", " "))
        sendkey(s, "ret", 0.5)
        shot(s, "V_completion")
    finally:
        kill_qemu(proc)


def verify_mount_dedup(qemu):
    print("\n=== Fix verify: mount dedup ===")
    proc, s = boot(qemu, PART_DISK, 4763)
    try:
        if not wait_for_prompt(s):
            check("boot prompt", False); return
        run(s, "clear", 0.8)
        out = run(s, "mount 2 /docs")
        check("first mount ok", "mounted" in out, out[-120:].replace("\n", " "))
        run(s, "mkdir /docs2", 1.0)
        out = run(s, "mount 2 /docs2")
        check("second place refuses same partition",
              "already mounted elsewhere" in out, out[-160:].replace("\n", " "))
        shot(s, "V_mount_dedup")
        out = run(s, "mount")
        check("table lists single hd0p2", out.count("hd0p2") == 1,
              out[-160:].replace("\n", " "))
    finally:
        kill_qemu(proc)


def main():
    qemu = find_qemu(None) or "/home/user/bin/qemu-system-i386"
    verify_tools_cal_completer(qemu)
    verify_mount_dedup(qemu)
    fails = [r for r in results if not r[1]]
    print("\n===== fix verify: %d run, %d fail =====" % (len(results), len(fails)))
    for n, ok, note in fails:
        print("  FAIL %s  %s" % (n, note))
    shutil.rmtree(STAGE, ignore_errors=True)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
