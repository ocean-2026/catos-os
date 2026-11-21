#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Quick single-command probe: raw 80x25 screen for `ls -l` (no whitespace folding)."""
import os, socket, subprocess, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qemu_driver import (find_qemu, kill_qemu, read_screen, sendkey,
                         type_text, wait_for_prompt, _stage_image)

REPO = os.path.dirname(HERE)
ISO = os.path.abspath(os.path.join(REPO, "kernel_v25", "catos.iso"))
PORT = 4713

def do(s, line, wait=1.0):
    type_text(s, line, 0.01); sendkey(s, "ret", wait)

def main():
    qemu = find_qemu(None)
    disk = os.path.join(HERE, "qemu_disk_v268.img")
    cmd = [qemu, "-m", "256", "-no-reboot",
           "-drive", "file=%s,format=raw,if=ide,index=0" % _stage_image(disk),
           "-drive", "file=%s,media=cdrom,if=ide,index=1" % _stage_image(ISO),
           "-boot", "order=d", "-display", "none", "-net", "none",
           "-monitor", "tcp:127.0.0.1:%d,server,nowait" % PORT]
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    s = None
    for _ in range(40):
        try:
            s = socket.create_connection(("127.0.0.1", PORT), timeout=5); break
        except OSError: time.sleep(2)
    try:
        time.sleep(18)
        type_text(s, "text", 0.05); sendkey(s, "ret", 0.6); sendkey(s, "y", 0.8)
        time.sleep(1.2)
        wait_for_prompt(s, timeout=120)
        do(s, "format", 1.2)
        sendkey(s, "y", 2.5)
        wait_for_prompt(s, timeout=30)
        do(s, "clear", 0.4)
        do(s, "ls -l", 1.5)
        print("=== RAW read_screen for `ls -l` ===")
        print(read_screen(s))
        print("=== END ===")
    finally:
        if s: s.close()
        kill_qemu(proc)
    return 0

if __name__ == "__main__":
    sys.exit(main())
