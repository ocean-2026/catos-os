#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Final v2.64 ls probe: format drive-0, seed a few files, verify all modes."""
import os, sys, time, subprocess, socket
sys.path.insert(0, os.path.dirname(__file__))
from qemu_driver import type_text, sendkey, read_screen, kill_qemu, _stage_image

QEMU = r"C:\qemu-portable\qemu-system-i386.exe"
ISO  = os.path.abspath(r"D:\CatOs\kernel_v25\catos.iso")
DISK = os.path.abspath(r"D:\CatOs\tests\qemu_disk_final.img")
PORT = 4640
if os.path.exists(DISK): os.remove(DISK)
with open(DISK,"wb") as f: f.truncate(64*1024*1024)
iso=_stage_image(ISO); disk=_stage_image(DISK)
cmdline=[QEMU,"-m","256","-no-reboot","-net","none",
    "-drive","file=%s,format=raw,if=ide,index=0"%disk,
    "-drive","file=%s,media=cdrom,if=ide,index=1"%iso,
    "-boot","order=d","-display","none",
    "-monitor","tcp:127.0.0.1:%d,server,nowait"%PORT]
proc=subprocess.Popen(cmdline,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
s=None
for _ in range(40):
    if proc.poll() is not None: raise RuntimeError("qemu rc=%r"%proc.returncode)
    try: s=socket.create_connection(("127.0.0.1",PORT),timeout=5); break
    except OSError: time.sleep(2)
def tail(n=14): return [r.rstrip() for r in read_screen(s).splitlines() if r.strip()][-n:]
try:
    time.sleep(24)
    type_text(s,"text",0.05); sendkey(s,"ret",0.6); sendkey(s,"y",0.8); time.sleep(1.2)
    type_text(s,"format",0.04); sendkey(s,"ret",0.6); time.sleep(0.6)
    sendkey(s,"y",0.15); sendkey(s,"ret",2.0); time.sleep(1.5)
    # seed a dir with a few files (few enough to fit one screen)
    for c in ["mkdir /tmp/td","touch /tmp/td/a.txt","touch /tmp/td/b.ce","touch /tmp/td/cbig"]:
        type_text(s,c,0.02); sendkey(s,"ret",0.4)
    def dump(command,label):
        type_text(s,"clear",0.02); sendkey(s,"ret",0.3)
        type_text(s,command,0.02); sendkey(s,"ret",0.8)
        lines=[r.rstrip() for r in read_screen(s).splitlines() if r.strip()]
        print("=== %s (%s) ==="%(label,command))
        for r in lines[:16]: print("  ",r)
    dump("ls /tmp/td","default")
    dump("ls -l /tmp/td","long")
    dump("ls -l --blocks=perm,size,name /tmp/td","blocks")
    dump("ls --color-scale /tmp/td","colorscale")
    dump("ls -t /tmp","tree")
    dump("dir /tmp/td","legacy-dir")
finally:
    kill_qemu(proc); print("DONE")
