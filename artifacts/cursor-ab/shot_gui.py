#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""临时：GUI 桌面截图（可选再注入鼠标点击序列），用于光标残影 A/B 目视取证。
用法: python shot_gui.py <iso> <outdir> [click]
"""
import os, sys, time, socket, struct, zlib, subprocess
HERE = r"D:\CatOs\tests"
sys.path.insert(0, HERE)
import qemu_driver as q

ISO = sys.argv[1]
OUT = sys.argv[2]
MODE = sys.argv[3] if len(sys.argv) > 3 else "shot"
os.makedirs(OUT, exist_ok=True)
PORT = 4571

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
    w, h, _ = vals
    px = d[i:i + w*h*3]
    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)
    raw = b"".join(b"\x00" + px[y*w*3:(y+1)*w*3] for y in range(h))
    open(png_path, "wb").write(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))
    return w, h

proc = subprocess.Popen([q.find_qemu(), "-m", "256", "-smp", "1", "-cpu", "qemu32",
                         "-vga", "std", "-no-reboot", "-net", "none", "-display", "none",
                         "-boot", "order=d", "-cdrom", ISO,
                         "-serial", "file:" + os.path.join(OUT, "serial.log"),
                         "-monitor", "tcp:127.0.0.1:%d,server,nowait" % PORT],
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
s = None
for _ in range(40):
    if proc.poll() is not None: raise RuntimeError("qemu rc=%r" % proc.returncode)
    try:
        s = socket.create_connection(("127.0.0.1", PORT), timeout=5); break
    except OSError: time.sleep(2)

n = [0]
def shot(label):
    n[0] += 1
    ppm = os.path.join(OUT, "s%02d.ppm" % n[0])
    q.cmd(s, "screendump " + ppm.replace("\\", "/"), 0.8)
    png = os.path.join(OUT, "s%02d_%s.png" % (n[0], label))
    print("SHOT", label, ppm_to_png(ppm, png), png)

try:
    q.force_text(s)
    if not q.wait_for_prompt(s, timeout=120):
        raise RuntimeError("no prompt")
    q.type_text(s, "gui"); q.sendkey(s, "ret", 1.2); time.sleep(3.5)
    q.cmd(s, "mouse_move -2000 -2000", 0.6)      # clamp to (0,0)
    if MODE == "shot":
        shot("desktop")
    elif MODE == "click":
        # 光标停在开始按钮上并点击，再点菜单第一项，观察光标下的重绘区域
        q.cmd(s, "mouse_move 26 586", 1.0)
        q.cmd(s, "mouse_button 1", 0.3)
        q.cmd(s, "mouse_button 0", 1.0)
        shot("menu_open")
        q.cmd(s, "mouse_move 40 500", 0.8)
        q.cmd(s, "mouse_button 1", 0.3)
        q.cmd(s, "mouse_button 0", 2.5)
        shot("after_item")
    elif MODE == "drag":
        # 拖动窗口：按下后连续移动，再松手截图
        q.cmd(s, "mouse_move 300 120", 0.8)
        q.cmd(s, "mouse_button 1", 0.3)
        for dx in (40, 40, 40, 40):
            q.cmd(s, "mouse_move %d %d" % (dx, 25), 0.25)
        shot("drag_mid")
        q.cmd(s, "mouse_button 0", 1.0)
        shot("drag_end")
finally:
    q.kill_qemu(proc)
    print("DONE")
