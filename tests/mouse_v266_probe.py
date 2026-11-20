#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""v2.66 mouse-sprite scroll probe.
Reproduces the user bug: pointer parked at the LEFT EDGE while long
output scrolls -- pre-fix the raw LFB sprite rode the pixel shift and
"ate" the first character of scattered rows.  Post-fix the arrow is
healed from the cell grid before the shift and redrawn in place.
"""
import os, sys, time, socket, struct, zlib, subprocess, json
sys.path.insert(0, os.path.dirname(__file__))
from qemu_driver import type_text, sendkey, kill_qemu, _stage_image, cmd

QEMU = r"C:\qemu-portable\qemu-system-i386.exe"
ISO  = os.path.abspath(r"D:\CatOs\kernel_v25\catos.iso")
DISK = os.path.abspath(r"D:\CatOs\tests\qemu_disk_cursor2.img")
OUT  = os.path.abspath(r"D:\CatOs\tests\shots_v266")
HMPORT, QMPORT = 4472, 4473
os.makedirs(OUT, exist_ok=True)
if os.path.exists(DISK): os.remove(DISK)
with open(DISK, "wb") as f: f.truncate(64 * 1024 * 1024)

def ppm_to_png(ppm_path, png_path):
    d = open(ppm_path, "rb").read()
    assert d[:2] == b"P6"
    vals = []; i = 2
    while len(vals) < 3:
        if d[i:i+1].isspace(): i += 1; continue
        if d[i:i+1] == b"#":
            while d[i:i+1] not in (b"\n", b""): i += 1
            continue
        j = i
        while not d[j:j+1].isspace(): j += 1
        vals.append(int(d[i:j])); i = j
    i += 1
    w, h = vals[0], vals[1]
    px = d[i:i + w*h*3]
    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)
    raw = b"".join(b"\x00" + px[y*w*3:(y+1)*w*3] for y in range(h))
    open(png_path, "wb").write(b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw, 6))
        + chunk(b"IEND", b""))

n_shot = [0]
def shot(label):
    n_shot[0] += 1
    ppm = os.path.join(OUT, "m%02d.ppm" % n_shot[0])
    cmd(s, "screendump " + ppm, 0.5)
    png = os.path.join(OUT, "m%02d_%s.png" % (n_shot[0], label))
    try:
        ppm_to_png(ppm, png); print("SHOT", label, "->", png)
    except Exception as e:
        print("SHOT %s FAILED: %s" % (label, e))

def do(line, wait=0.9):
    type_text(s, line, 0.02); sendkey(s, "ret", wait); time.sleep(wait * 0.3)

qmp = None
iso = _stage_image(ISO); disk = _stage_image(DISK)
cmdline = [QEMU, "-m", "256", "-no-reboot", "-net", "none",
    "-drive", "file=%s,format=raw,if=ide,index=0" % disk,
    "-drive", "file=%s,media=cdrom,if=ide,index=1" % iso,
    "-boot", "order=d", "-display", "none",
    "-monitor", "tcp:127.0.0.1:%d,server,nowait" % HMPORT,
    "-qmp", "tcp:127.0.0.1:%d,server,nowait" % QMPORT]
proc = subprocess.Popen(cmdline, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
s = None
for _ in range(40):
    if proc.poll() is not None: raise RuntimeError("qemu rc=%r" % proc.returncode)
    try:
        s = socket.create_connection(("127.0.0.1", HMPORT), timeout=5); break
    except OSError: time.sleep(2)
for _ in range(40):
    try:
        qmp = socket.create_connection(("127.0.0.1", QMPORT), timeout=5); break
    except OSError: time.sleep(2)

def qmp_exec(d):
    qmp.sendall((json.dumps(d) + "\n").encode())
    time.sleep(0.05)
    try:
        return qmp.recv(65536).decode(errors="replace")
    except OSError:
        return ""

def mouse_move(dx, dy):
    evs = []
    if dx: evs.append({"type": "rel", "data": {"axis": "x", "value": dx}})
    if dy: evs.append({"type": "rel", "data": {"axis": "y", "value": dy}})
    if evs: qmp_exec({"execute": "input-send-event", "arguments": {"events": evs}})

try:
    r = qmp_exec({"execute": "qmp_capabilities"})
    print("QMP:", r.replace("\n", " ")[:80])
    time.sleep(24)                    # boot to gfx prompt
    do("format", 0.6); sendkey(s, "y", 0.2); sendkey(s, "ret", 2.0); time.sleep(1.0)

    # long output scrolls WHILE the pointer sits on the text area.
    # NOTE: 'clear' calls mc_invalidate() which erases the arrow until the
    # next mouse move, so park the arrow AFTER clear, then scroll.
    do("clear", 0.5)
    mouse_move(-256, 0); time.sleep(0.3)
    mouse_move(0, -256); time.sleep(0.3)
    mouse_move(0, 100);  time.sleep(0.5)
    shot("mouse_parked_over_text")
    type_text(s, "ls /bin", 0.02); sendkey(s, "ret", 0.1)
    mouse_move(0, 3); time.sleep(0.2)      # nudge: keep arrow alive
    time.sleep(2.0)
    shot("after_scroll_mouse_over_text")

    # now wiggle the pointer DURING output (save/restore race)
    do("clear", 0.6)
    type_text(s, "ls /bin", 0.02); sendkey(s, "ret", 0.05)
    for _ in range(6):
        mouse_move(3, -6); time.sleep(0.25)
        mouse_move(-3, 6); time.sleep(0.25)
    time.sleep(1.5)
    shot("after_scroll_mouse_wiggle")
finally:
    try: qmp_exec({"execute": "quit"})
    except Exception: pass
    kill_qemu(proc)
    print("DONE")
