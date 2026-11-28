#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""final3 声明语法取证：CE rec2 记录槽 img_off/icon_sz 落地为块 img/icon_sz。

场景（独立开机）：gui shot-decl-img
  btn0 声明 img=/icons/ok.svg icon=16  -> 文件 SVG 出按钮（与内核内构建一致）。
  btn1 声明 img=/nope/x.svg           -> 回落内置 cancel，串口恰一条 fallback。
  btn2 声明 img=/icons/ok.svg icon=99 -> 非法档位被忽略，按钮正常渲染（默认 16 档）。

磁盘：宿主播种 /icons/ok.svg；/nope 故意不存在。
"""
import os, sys, subprocess, time
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import qemu_driver as q
from PIL import Image
from desk_r4_probe import Seeder, lfn_entries, alias_for, PART_START, ROOT_CLUS
import struct

ISO = os.path.join(REPO, "kernel_v25", "catos.iso")
MKDISK = os.path.join(REPO, "kernel_v25", "build", "mkdisk.py")
ART = os.path.join(REPO, "artifacts", "desktop-icons", "final3")
TMP = "/tmp/final3_decl"
os.makedirs(ART, exist_ok=True)
os.makedirs(TMP, exist_ok=True)

OK_SVG = (
    '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 16 16">\n'
    '<rect x="1" y="1" width="14" height="14" fill="#4C8DFF"/>\n'
    '<path d="M4.5 8.2 L6.8 10.5 L11.5 5.5 L12.5 6.5 L6.8 12 L3.5 8.7 Z" fill="#FFFFFF"/>\n'
    '</svg>\n'
).encode("utf-8")


def build_disk():
    base = os.path.join(TMP, "base.img")
    if not os.path.exists(base):
        subprocess.run([sys.executable, MKDISK, "-o", base, "-m", "64", "--no-files"],
                       check=True, capture_output=True)
    out = os.path.join(TMP, "decl.img")
    subprocess.run(["cp", base, out], check=True)
    sd = Seeder(out)
    icons_clus = sd.alloc()
    buf = bytearray(sd.csz)
    d = bytearray(32); d[0] = ord('.'); d[11] = 0x10
    struct.pack_into("<H", d, 26, icons_clus & 0xFFFF)
    d2 = bytearray(32)
    d2[0] = ord('.'); d2[1] = ord('.'); d2[11] = 0x10
    struct.pack_into("<H", d2, 26, ROOT_CLUS & 0xFFFF)
    struct.pack_into("<H", d2, 20, ROOT_CLUS >> 16)
    buf[0:32] = bytes(d); buf[32:64] = bytes(d2)
    sd.img[sd.clus_off(icons_clus):sd.clus_off(icons_clus) + sd.csz] = buf
    slot = sd.free_slot(ROOT_CLUS)
    for ent in lfn_entries("icons", alias_for("icons", 1), 0x10, icons_clus, 0):
        sd.img[slot:slot + 32] = ent
        slot = sd.free_slot(ROOT_CLUS)
    clus = sd.write_file(OK_SVG)
    for ent in lfn_entries("ok.svg", alias_for("ok.svg", 2), 0x20, clus, len(OK_SVG)):
        s = sd.free_slot(icons_clus)
        sd.img[s:s + 32] = ent
    sd.finalize(out)
    print("seeded decl disk ->", out)
    return out


def shoot(s, png):
    ppm = png.replace(".png", ".ppm")
    if os.path.exists(ppm): os.remove(ppm)
    q.cmd(s, "screendump " + ppm, 1.0)
    dl = time.time() + 10
    while time.time() < dl:
        if os.path.exists(ppm) and os.path.getsize(ppm) > 0: break
        time.sleep(0.3)
    Image.open(ppm).convert("RGB").save(png)
    print("  SHOT", png)


def boot(disk, port, serial):
    qemu = q.find_qemu(None) or os.path.expanduser("~/bin/qemu-system-i386")
    proc, s = q.boot(qemu, ISO, port, disk=disk,
                     extra_args=["-smp", "1", "-cpu", "qemu32", "-vga", "vmware",
                                 "-serial", "file:" + serial])
    q.force_text(s, settle=10)
    if not q.wait_for_prompt(s, timeout=90):
        raise RuntimeError("no prompt")
    return proc, s


DISK = build_disk()
ser = os.path.join(ART, "serial-decl-img.log")
if os.path.exists(ser): os.remove(ser)
proc, s = boot(DISK, 4735, ser)
try:
    q.type_text(s, "gui shot-decl-img", 0.05); q.sendkey(s, "ret", 1.0)
    time.sleep(6.0)
    full = os.path.join(ART, "decl-img-full.png")
    shoot(s, full)
    img = Image.open(full)
    # 实机行位（CE 列布局，moved 绝对位未完全覆盖；按整帧扫描测得）。
    tops = [6, 39, 72]
    crops = {0: "btn-decl-svg.png", 1: "btn-decl-fallback.png",
             2: "btn-decl-badicon.png"}
    for di, name in crops.items():
        y = tops[di]
        img.crop((0, y - 3, 224, y + 35)).save(os.path.join(ART, name))
        print("  CROP", name)
finally:
    q.kill_qemu(proc)

print("== decl-img log ==")
for ln in open(ser, errors="replace"):
    if "GUI:decl-img" in ln or "fallback" in ln:
        print("  ", ln.strip())
bad = [l.strip() for l in open(ser, errors="replace") if "fallback" in l]
print("fallback count=", len(bad))
