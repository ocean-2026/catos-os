#!/usr/bin/env python3
"""review_root_remount.py -- verify the root partition cannot be re-bound.

#142 originally deduped only the extra-volume mount table; the root volume
(slot 0) was still mountable a second time under another point, opening a
second FAT cache over the same LBA.  This probe boots the two-partition disk,
tries to bind P1 (the boot/root volume) to /docs2, and expects refusal; then
re-checks the P2 duplicate-mount refusal still works (regression).
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

REPO = os.path.dirname(HERE)
ISO = os.path.join(REPO, "kernel_v25", "catos.iso")
PART_DISK = os.path.join(HERE, "qemu_disk_part.img")
STAGE = os.path.join(tempfile.gettempdir(), "catos_root_stage")
os.makedirs(STAGE, exist_ok=True)
DISK = os.path.join(STAGE, "part.img")

fails = []


def check(name, ok, note=""):
    print("  %-4s %-30s %s" % ("PASS" if ok else "FAIL", name, note[:150]))
    if not ok:
        fails.append(name)


def run(s, line, wait=1.2):
    type_text(s, line)
    sendkey(s, "ret", wait)
    time.sleep(0.4)
    return screen_text(s)


def main():
    qemu = find_qemu(None) or "/home/user/bin/qemu-system-i386"
    shutil.copy2(PART_DISK, DISK)
    proc = subprocess.Popen(
        [qemu, "-m", "256", "-no-reboot", "-net", "none", "-cdrom", ISO,
         "-boot", "order=d", "-display", "none",
         "-monitor", "tcp:127.0.0.1:4776,server,nowait",
         "-drive", "file=%s,format=raw,if=ide" % DISK],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        s = None
        for _ in range(40):
            if proc.poll() is not None:
                raise RuntimeError("qemu exited rc=%r" % proc.returncode)
            try:
                s = socket.create_connection(("127.0.0.1", 4776), timeout=5)
                break
            except OSError:
                time.sleep(1)
        if s is None:
            raise RuntimeError("no monitor")
        if not wait_for_prompt(s):
            check("boot prompt", False); return 1

        run(s, "mkdir /docs2", 0.8)
        run(s, "mkdir /docs3", 0.8)

        out = run(s, "mount 1 /docs2")
        check("root partition re-mount refused",
              "already mounted" in out, out[-140:].replace("\n", " "))

        out = run(s, "mount 2 /docs")
        check("P2 still mounts", "mounted" in out, out[-140:].replace("\n", " "))

        out = run(s, "mount 2 /docs3")
        check("P2 duplicate still refused",
              "already mounted" in out, out[-140:].replace("\n", " "))

        run(s, "clear", 0.8)
        out = run(s, "mount")
        check("mount table lists only P2", "/docs2" not in out and "/docs3" not in out,
              out[-160:].replace("\n", " "))
        run(s, "umount /docs")
    finally:
        kill_qemu(proc)
        shutil.rmtree(STAGE, ignore_errors=True)

    print("\n===== root remount: %d fail =====" % len(fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
