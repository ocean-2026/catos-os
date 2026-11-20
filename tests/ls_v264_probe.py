#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""v2.64 ls redesign QEMU probe.

KEY FIX vs earlier probes: CatOS fat_probe() hard-codes IDE drive 0.
Default boot() puts the ISO at 0:0 and the disk at 0:1, so FAT32 is never
found.  Here we pin the disk to index=0 and the CD to index=1, so drive 0
is the writable disk.  We format it in-guest, then exercise all six ls modes.
"""
import os, sys, time, subprocess, socket
sys.path.insert(0, os.path.dirname(__file__))
from qemu_driver import cmd, type_text, sendkey, read_screen, kill_qemu, _stage_image

QEMU = r"C:\qemu-portable\qemu-system-i386.exe"
ISO = os.path.abspath(r"D:\CatOs\kernel_v25\catos.iso")
DISK = os.path.abspath(r"D:\CatOs\tests\qemu_disk_v264_probe.img")
PORT = 4630

# fresh 64 MiB disk
if os.path.exists(DISK):
    os.remove(DISK)
with open(DISK, "wb") as f:
    f.truncate(64 * 1024 * 1024)

iso = _stage_image(ISO)
disk = _stage_image(DISK)
cmdline = [
    QEMU, "-m", "256", "-no-reboot", "-net", "none",
    "-drive", "file=%s,format=raw,if=ide,index=0" % disk,
    "-drive", "file=%s,media=cdrom,if=ide,index=1" % iso,
    "-boot", "order=d", "-display", "none",
    "-monitor", "tcp:127.0.0.1:%d,server,nowait" % PORT,
]
proc = subprocess.Popen(cmdline, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
s = None
for _ in range(40):
    if proc.poll() is not None:
        raise RuntimeError("qemu exited rc=%r" % proc.returncode)
    try:
        s = socket.create_connection(("127.0.0.1", PORT), timeout=5)
        break
    except OSError:
        time.sleep(2)
if s is None:
    kill_qemu(proc)
    raise RuntimeError("no monitor")

def screen_tail(n=14):
    return [r.rstrip() for r in read_screen(s).splitlines() if r.strip()][-n:]

try:
    time.sleep(24)
    # blind switch to text (memory convention: text + Enter, then physical y)
    type_text(s, "text", 0.05); sendkey(s, "ret", 0.6); sendkey(s, "y", 0.8)
    time.sleep(1.2)
    print("TEXT_OK=", bool(read_screen(s).strip()))

    # format the now-drive-0 disk
    type_text(s, "format", 0.04); sendkey(s, "ret", 0.6); time.sleep(0.6)
    sendkey(s, "y", 0.15); sendkey(s, "ret", 2.0); time.sleep(1.5)
    print("=== after format ===")
    for r in screen_tail(6): print(" ", r)

    def dump(command, label):
        type_text(s, "clear", 0.03); sendkey(s, "ret", 0.4)
        type_text(s, command, 0.03); sendkey(s, "ret", 1.2)
        lines = [r.rstrip() for r in read_screen(s).splitlines() if r.strip()]
        print("=== %s (%s) [%d lines] ===" % (label, command, len(lines)))
        for r in lines[:18]: print(" ", r)

    dump("ls /bin", "default")
    dump("ls -l /bin", "long")
    dump("ls -t /etc", "tree")
    dump("ls -lt /etc", "treelong")
    dump("ls -l --blocks=perm,size,name /bin", "blocks")
    dump("ls --color-scale /bin", "colorscale")
finally:
    kill_qemu(proc)
    print("DONE")
