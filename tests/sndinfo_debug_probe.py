#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Debug probe: raw screen after sndinfo variants and redirect sanity check."""
import os, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qemu_driver import (boot, find_qemu, kill_qemu, read_screen,
                         sendkey, type_text, wait_for_prompt)

ISO = os.path.abspath(os.path.join(os.path.dirname(HERE), "kernel_v25", "catos.iso"))
AUDIO = ["-audiodev", "wav,id=snd0,path=%s" % os.path.join(HERE, "qemu_shots", "dbg.wav"),
         "-device", "sb16,iobase=0x220,irq=5,dma=1,dma16=5,audiodev=snd0"]
proc, s = boot(find_qemu(None), ISO, 4666, extra_args=AUDIO)

def do(line, wait=1.5):
    type_text(s, line, 0.02); sendkey(s, "ret", wait)

def show(label):
    print("=== %s ===" % label)
    for i, r in enumerate(read_screen(s).splitlines()):
        if r.strip(): print("%2d |%s" % (i, r.rstrip()))

try:
    time.sleep(20)
    type_text(s, "text", 0.05); sendkey(s, "ret", 0.6)
    sendkey(s, "y", 0.8); time.sleep(1.2)
    if not wait_for_prompt(s, timeout=120.0): raise SystemExit("no prompt")
    do("format", 1.0); sendkey(s, "y", 0.4); sendkey(s, "ret", 15.0)
    if not wait_for_prompt(s, timeout=90.0): raise SystemExit("no prompt after format")

    do("echo hello > /tmp/z.txt", 1.2)
    do("cat /tmp/z.txt", 1.2); show("redirect /tmp/z.txt")
    do("echo hello2 > z2.txt", 1.2)
    do("cat z2.txt", 1.2); show("redirect z2.txt")
    do("sndinfo -h", 3.0); show("sndinfo -h raw")
    do("sndinfo", 3.0); show("sndinfo default raw")
finally:
    kill_qemu(proc)
