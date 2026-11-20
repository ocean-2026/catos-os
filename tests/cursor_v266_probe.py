#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""v2.66 probe: gfx-mode screendump verification for
  1. cursor char-loss during multi-line output (ls /bin scrolling)
  2. long-command soft wrap
  3. PgDn line-by-line scrollback (after PgUp)
Boot stays in the DEFAULT gfx mode (no blind 'text' switch needed);
all typing is blind, evidence comes from HMP screendumps -> PNG.
"""
import os, sys, time, socket, struct, zlib, subprocess
sys.path.insert(0, os.path.dirname(__file__))
from qemu_driver import type_text, sendkey, kill_qemu, _stage_image

QEMU = r"C:\qemu-portable\qemu-system-i386.exe"
ISO  = os.path.abspath(r"D:\CatOs\kernel_v25\catos.iso")
DISK = os.path.abspath(r"D:\CatOs\tests\qemu_disk_cursor.img")
OUT  = os.path.abspath(r"D:\CatOs\tests\shots_v266")
PORT = 4470
os.makedirs(OUT, exist_ok=True)
if os.path.exists(DISK): os.remove(DISK)
with open(DISK, "wb") as f: f.truncate(64 * 1024 * 1024)

def ppm_to_png(ppm_path, png_path):
    d = open(ppm_path, "rb").read()
    # parse P6 header (whitespace/newline separated tokens, one comment line max)
    assert d[:2] == b"P6", "not P6"
    vals = []; i = 2
    while len(vals) < 3:
        if d[i:i+1].isspace(): i += 1; continue
        if d[i:i+1] == b"#":
            while d[i:i+1] not in (b"\n", b""): i += 1
            continue
        j = i
        while not d[j:j+1].isspace(): j += 1
        vals.append(int(d[i:j])); i = j
    i += 1  # single whitespace after maxval
    w, h, maxv = vals[0], vals[1], vals[2]
    px = d[i:i + w*h*3]
    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)
    raw = b"".join(b"\x00" + px[y*w*3:(y+1)*w*3] for y in range(h))
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 6))
           + chunk(b"IEND", b""))
    open(png_path, "wb").write(png)
    return w, h

iso = _stage_image(ISO); disk = _stage_image(DISK)
cmdline = [QEMU, "-m", "256", "-no-reboot", "-net", "none",
    "-drive", "file=%s,format=raw,if=ide,index=0" % disk,
    "-drive", "file=%s,media=cdrom,if=ide,index=1" % iso,
    "-boot", "order=d", "-display", "none",
    "-monitor", "tcp:127.0.0.1:%d,server,nowait" % PORT]
proc = subprocess.Popen(cmdline, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
s = None
for _ in range(40):
    if proc.poll() is not None: raise RuntimeError("qemu rc=%r" % proc.returncode)
    try:
        s = socket.create_connection(("127.0.0.1", PORT), timeout=5); break
    except OSError: time.sleep(2)

n_shot = [0]
def shot(label):
    n_shot[0] += 1
    ppm = os.path.join(OUT, "s%02d.ppm" % n_shot[0])
    from qemu_driver import cmd
    cmd(s, "screendump " + ppm, 0.5)
    png = os.path.join(OUT, "s%02d_%s.png" % (n_shot[0], label))
    try:
        w, h = ppm_to_png(ppm, png)
        print("SHOT %s -> %s (%dx%d)" % (label, png, w, h))
    except Exception as e:
        print("SHOT %s FAILED: %s" % (label, e))

def do(line, wait=0.9):
    type_text(s, line, 0.02)
    sendkey(s, "ret", wait)
    time.sleep(wait * 0.3)

try:
    time.sleep(24)                    # boot to gfx prompt
    do("format", 0.6); sendkey(s, "y", 0.2); sendkey(s, "ret", 2.0); time.sleep(1.0)

    # --- 1. cursor char-loss: long output scrolls in gfx mode ---
    do("clear", 0.5)
    do("ls /bin", 2.5)
    time.sleep(1.0)
    shot("ls_bin_after")

    # scroll up one page, then PgDn: should come back line-by-line,
    # one press = one page worth of line steps ending at the live view
    sendkey(s, "pgup", 0.3); time.sleep(1.5)
    shot("ls_bin_pgup")
    sendkey(s, "pgdn", 0.3); time.sleep(0.4)   # mid-animation
    shot("ls_bin_pgdn_mid")
    time.sleep(1.5)
    shot("ls_bin_pgdn_done")

    # --- 2. long command soft wrap ---
    do("clear", 0.5)
    long_cmd = "echo aaaaaaaaaa bbbbbbbbbb cccccccccc dddddddddd eeeeeeeeee ffffffffff gggggggggg hhhhhhhhhh"
    type_text(s, long_cmd, 0.02)
    time.sleep(1.0)
    shot("wrap_typed")
    sendkey(s, "ret", 1.0)
    time.sleep(0.5)
    shot("wrap_done")
finally:
    kill_qemu(proc)
    print("DONE")
