#!/usr/bin/env python3
"""review_touch.py -- interactive metal check: does `touch` create a resolvable
empty file on a fresh boot?  Used to diagnose the Boot A `wc empty file` FAIL.
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
from qemu_driver import find_qemu, kill_qemu, screen_text, sendkey, type_text, wait_for_prompt  # noqa: E402

REPO = os.path.dirname(HERE)
ISO = os.path.join(REPO, "kernel_v25", "catos.iso")
SRC = os.path.join(REPO, "kernel_v25", "catos_disk.img")
STAGE = os.path.join(tempfile.gettempdir(), "catos_review_touch")


def main():
    qemu = find_qemu(None) or "/home/user/bin/qemu-system-i386"
    os.makedirs(STAGE, exist_ok=True)
    disk = os.path.join(STAGE, "t.img")
    shutil.copy2(SRC, disk)
    cmd = [qemu, "-m", "256", "-no-reboot", "-net", "none", "-cdrom", ISO,
           "-boot", "order=d", "-display", "none",
           "-monitor", "tcp:127.0.0.1:4754,server,nowait",
           "-drive", "file=%s,format=raw,if=ide" % disk]
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        s = None
        for _ in range(40):
            if proc.poll() is not None:
                raise RuntimeError("qemu exited")
            try:
                s = socket.create_connection(("127.0.0.1", 4754), timeout=5)
                break
            except OSError:
                time.sleep(1)
        if not wait_for_prompt(s):
            print("NO PROMPT"); return 2

        def run(line, wait=1.2):
            type_text(s, line)
            sendkey(s, "ret", wait)
            time.sleep(0.4)
            return screen_text(s)

        print("--- touch /tmp/empty.txt ---")
        print(run("touch /tmp/empty.txt"))
        print("--- ls /tmp ---")
        print(run("ls /tmp"))
        print("--- wc /tmp/empty.txt ---")
        print(run("wc /tmp/empty.txt"))
        print("--- touch /root/e.txt ---")
        print(run("touch /root/e.txt"))
        print("--- ls /root ---")
        print(run("ls /root"))
        print("--- mkdir /tmp/d1 ---")
        print(run("mkdir /tmp/d1"))
        print("--- ls /tmp ---")
        print(run("ls /tmp"))
    finally:
        kill_qemu(proc)
        shutil.rmtree(STAGE, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
