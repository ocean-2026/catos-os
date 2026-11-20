#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""v2.65 sndinfo probe: POSIX options, all output modes, exit codes, tone.
Boots with a real QEMU SB16 so the mixer/PCM paths use live hardware state."""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qemu_driver import (boot, find_qemu, kill_qemu, screen_text,
                         sendkey, type_text, wait_for_prompt)

REPO = os.path.dirname(HERE)
ISO = os.path.abspath(os.path.join(REPO, "kernel_v25", "catos.iso"))
PORT = 4665
AUDIO = [
    "-audiodev", "wav,id=snd0,path=%s" % os.path.join(HERE, "qemu_shots", "sndinfo265.wav"),
    "-device", "sb16,iobase=0x220,irq=5,dma=1,dma16=5,audiodev=snd0",
]

os.makedirs(os.path.join(HERE, "qemu_shots"), exist_ok=True)
qemu = find_qemu(None)
if not qemu:
    raise SystemExit("qemu not found")
DISK = os.path.join(HERE, "qemu_disk_sndinfo.img")
if not os.path.exists(DISK):
    with open(DISK, "wb") as fh: fh.truncate(64 * 1024 * 1024)
proc, s = boot(qemu, ISO, PORT, disk=DISK, extra_args=AUDIO)
results = []

def do(line, wait=1.2):
    type_text(s, line, 0.01)
    sendkey(s, "ret", wait)

def wait_text(needle, timeout=12.0):
    deadline = time.time() + timeout
    best = ""
    while time.time() < deadline:
        best = screen_text(s)
        if needle in best:
            return best
        time.sleep(0.5)
    return best

def dump(label, line, needle, wait=1.5):
    do("clear", 0.8)
    do(line, wait)
    txt = wait_text(needle)
    print("=== %s : %s ===" % (label, line))
    for r in [x.rstrip() for x in txt.splitlines() if x.strip()][:18]:
        print("  ", r)
    return txt

def dump_via_file(label, line, path, needle, head_lines=8):
    """Long output scrolls past 25 rows in the QEMU text backend; capture
    it to FAT and read back a short prefix instead."""
    do("clear", 0.6)
    do("%s > %s" % (line, path), 1.5)
    do("clear", 0.5)
    do("head -n %d %s" % (head_lines, path), 1.5)
    txt = wait_text(needle)
    print("=== %s (via %s) ===" % (label, path))
    for r in [x.rstrip() for x in txt.splitlines() if x.strip()][:14]:
        print("  ", r)
    return txt

def echo_rc():
    """qemu_driver maps '?' to shift-dot (wrong on US layout); use shift-slash."""
    type_text(s, "echo $", 0.02)
    sendkey(s, "shift-slash", 0.02)
    sendkey(s, "ret", 1.0)
    time.sleep(0.4)
    return screen_text(s)

try:
    # Graphics mode is the boot default; blindly switch to text before
    # reading the VGA text buffer (the monitor cannot see the framebuffer).
    time.sleep(20)
    type_text(s, "text", 0.05); sendkey(s, "ret", 0.6)
    sendkey(s, "y", 0.8); time.sleep(1.2)
    if not wait_for_prompt(s, timeout=120.0):
        raise RuntimeError("shell prompt never appeared")
    do("format", 1.0); sendkey(s, "y", 0.4); sendkey(s, "ret", 15.0)
    if not wait_for_prompt(s, timeout=90.0):
        raise RuntimeError("shell never returned after format")

    txt = dump("default", "sndinfo", "Mic Boost")
    results.append(("default list", "SB16 ready" in txt and "pcm0p" in txt))
    txt = dump("help", "sndinfo -h", "Exit: 0 ok")
    results.append(("-h help", "--json" in txt and "--watch" in txt))
    txt = dump("version", "sndinfo --version", "0.65.0")
    results.append(("-v version", "0.65.0" in txt))
    txt = dump("pcm", "sndinfo -p", "Buffer size:")
    results.append(("-p pcm", "State:" in txt and "Buffer size:" in txt))
    txt = dump("ctrl", "sndinfo --ctrl", "Master Playback")
    results.append(("-c ctrl", "Master Playback" in txt and "%" in txt))
    txt = dump("short", "sndinfo -s", "pcm0p:play")
    results.append(("-s short", "pcm0p:play" in txt))
    txt = dump("json", "sndinfo -j", "card_id")
    results.append(("-j json", '"card_id"' in txt and '"pcm"' in txt))
    txt = dump("dev0", "sndinfo --dev=0 -s", "pcm0p:play")
    results.append(("--dev=0", "pcm0p:play" in txt))
    txt = dump("combined", "sndinfo -lp", "State:")
    results.append(("-lp cluster", "Mic Boost" in txt and "State:" in txt))
    txt = dump("bad card", "sndinfo -d 9", "does not exist")
    results.append(("-d 9 message", "does not exist" in txt))
    do("clear", 0.8); do("sndinfo -d 9", 0.8)
    txt = echo_rc()
    results.append(("exit 3 via $?", "echo $? 3" in txt))
    print("=== $? probe ===\n", txt[-500:])
    txt = dump("bad option", "sndinfo --bogus", "unknown option")
    results.append(("--bogus", "unknown option" in txt))
    txt = dump("quiet", "sndinfo -q -d 9", "root@catos")
    results.append(("-q quiet", "does not exist" not in txt))
    txt = dump("tone", "sndinfo -t", "played", 3.0)
    results.append(("-t tone", "played" in txt or "880" in txt))
finally:
    kill_qemu(proc)

print("\n========== SUMMARY ==========")
for name, ok in results:
    print("%-4s %s" % ("PASS" if ok else "FAIL", name))
print("%d/%d passed" % (sum(1 for _, ok in results if ok), len(results)))
