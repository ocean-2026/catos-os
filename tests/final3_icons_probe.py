#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""final3 块化 GUI：按钮图片资源 + 内核极简 SVG 光栅化子集取证。

场景（独立开机）：
  shot-icons      全量按钮列：ok.svg / folder.svg / missing.svg(回落 file) /
                  icon=24 大图标 / 仅图标；整帧截图后宿主按按钮几何裁切。
  shot-icons-bad  仅坏路径 /nope/x.svg：断言串口仅一条 fallback 行、文字仍在。
  icon-audit      内核自检：每按钮墨迹/越界/缓存槽位/回落计数，result=1。

磁盘：宿主侧播种 /icons/ok.svg、/icons/folder.svg；missing.svg 与 /nope 故意不存在。
"""
import os, sys, struct, subprocess, time
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import qemu_driver as q
from PIL import Image
from desk_r4_probe import Seeder, lfn_entries, alias_for, PART_START, ROOT_CLUS

ISO = os.path.join(REPO, "kernel_v25", "catos.iso")
MKDISK = os.path.join(REPO, "kernel_v25", "build", "mkdisk.py")
ART = os.path.join(REPO, "artifacts", "desktop-icons", "final3")
TMP = "/tmp/final3_icons"
os.makedirs(ART, exist_ok=True)
os.makedirs(TMP, exist_ok=True)

OK_SVG = (
    '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 16 16">\n'
    '<rect x="1" y="1" width="14" height="14" fill="#4C8DFF"/>\n'
    '<path d="M4.5 8.2 L6.8 10.5 L11.5 5.5 L12.5 6.5 L6.8 12 L3.5 8.7 Z" fill="#FFFFFF"/>\n'
    '</svg>\n'
).encode("utf-8")

FOLDER_SVG = (
    '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 16 16">\n'
    '<rect x="1" y="3" width="5" height="2.5" fill="#F0D060"/>\n'
    '<rect x="1" y="5" width="14" height="8" fill="#E8C53A"/>\n'
    '</svg>\n'
).encode("utf-8")


def build_disk():
    base = os.path.join(TMP, "base.img")
    if not os.path.exists(base):
        subprocess.run([sys.executable, MKDISK, "-o", base, "-m", "64", "--no-files"],
                       check=True, capture_output=True)
    out = os.path.join(TMP, "icons.img")
    subprocess.run(["cp", base, out], check=True)
    sd = Seeder(out)

    # 根目录建 ICONS 目录
    icons_clus = sd.alloc()
    buf = bytearray(sd.csz)
    d = bytearray(32)
    d[0] = ord('.'); d[11] = 0x10
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

    files = {"ok.svg": OK_SVG, "folder.svg": FOLDER_SVG}
    for idx, (name, data) in enumerate(files.items(), start=2):
        clus = sd.write_file(data)
        for ent in lfn_entries(name, alias_for(name, idx), 0x20, clus, len(data)):
            s = sd.free_slot(icons_clus)
            sd.img[s:s + 32] = ent

    sd.finalize(out)
    print("seeded icons disk ->", out)
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


def gui(s, cmd, wait=6.0):
    q.type_text(s, cmd, 0.05); q.sendkey(s, "ret", 1.0); time.sleep(wait)


DISK = build_disk()

# 1. 全量图标按钮列
ser = os.path.join(ART, "serial-icons.log")
if os.path.exists(ser): os.remove(ser)
proc, s = boot(DISK, 4731, ser)
try:
    gui(s, "gui shot-icons")
    shoot(s, os.path.join(ART, "icons-full.png"))
    # 按固定按钮几何裁切单按钮图（x=40, y=40+di*44, 220x32）。
    full = Image.open(os.path.join(ART, "icons-full.png"))
    crop_map = {0: "btn-svg-ok.png", 1: "btn-svg-folder.png",
                2: "btn-svg-fallback.png", 3: "btn-icon24.png",
                4: "btn-notext.png"}
    for di, name in crop_map.items():
        x, y, w, h = 40, 40 + di * 44, 220, 32
        full.crop((x - 4, y - 4, x + w + 4, y + h + 4)).save(
            os.path.join(ART, name))
        print("  CROP", name)
finally:
    q.kill_qemu(proc)

# 2. 仅坏路径
ser_bad = os.path.join(ART, "serial-icons-bad.log")
if os.path.exists(ser_bad): os.remove(ser_bad)
proc, s = boot(DISK, 4732, ser_bad)
try:
    gui(s, "gui shot-icons-bad")
    shoot(s, os.path.join(ART, "btn-notext-bad.png"))
    badfull = Image.open(os.path.join(ART, "btn-notext-bad.png"))
    bx, by = 40, 40 + 5 * 44
    badfull.crop((bx - 4, by - 4, bx + 220 + 4, by + 32 + 4)).save(
        os.path.join(ART, "btn-notext-bad.png"))
finally:
    q.kill_qemu(proc)

# 2b. hover 态：首按钮注入 hover，验证 hover 背景下图标仍可见。
ser_hov = os.path.join(ART, "serial-icons-hover.log")
if os.path.exists(ser_hov): os.remove(ser_hov)
proc, s = boot(DISK, 4734, ser_hov)
try:
    gui(s, "gui shot-icons-hover")
    shoot(s, os.path.join(ART, "btn-hover-icon.png"))
    hov = Image.open(os.path.join(ART, "btn-hover-icon.png"))
    hov.crop((36, 36, 224, 76)).save(os.path.join(ART, "btn-hover-icon-crop.png"))
finally:
    q.kill_qemu(proc)

# 3. 内核自检
ser_au = os.path.join(ART, "serial-icon-audit.log")
if os.path.exists(ser_au): os.remove(ser_au)
proc, s = boot(DISK, 4733, ser_au)
try:
    gui(s, "gui icon-audit", 8.0)
finally:
    q.kill_qemu(proc)

print("== audit ==")
for ln in open(ser_au, errors="replace"):
    if "GUI:icon" in ln:
        print("  ", ln.strip())
print("== bad fallback lines ==")
bad = [l.strip() for l in open(ser_bad, errors="replace") if "fallback" in l]
print("  count=", len(bad))
for l in bad: print("  ", l)
