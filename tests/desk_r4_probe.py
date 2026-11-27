#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""R4 功能核查探针：/Desktop 图标列（v2.82 R3）。

三场景独立开机：
  six    6 条目（1 目录 / 1 txt / 1 bmp / 1 纯中文长名 / 1 中英混排长名 /
         1 纯 ASCII 长名）-> desk-r4.png
  empty  空桌面 -> desk-empty-r4.png
  light  six 条目 + light 令牌 -> desk-light-r4.png
  eight  8 条目（含 1 未知扩展名）-> 核查上限 6、越界安全、未知扩展名无副作用

播种在宿主侧完成（VFAT LFN 写法对照 kernel_v25/src/fat.c dir_put_run）。

用法:
  python3 tests/desk_r4_probe.py [scenario ...]
"""
import os
import re
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from qemu_driver import (boot, find_qemu, kill_qemu, force_text, wait_for_prompt,
                         sendkey, type_text, cmd, read_screen)

ISO = os.path.join(REPO, "kernel_v25", "catos.iso")
MKDISK = os.path.join(REPO, "kernel_v25", "build", "mkdisk.py")
ART = os.path.join(REPO, "artifacts", "desktop-icons")
TMP = "/tmp/desk_r4"
os.makedirs(ART, exist_ok=True)
os.makedirs(TMP, exist_ok=True)

PART_START = 1
RESERVED = 32
NUM_FATS = 2
ROOT_CLUS = 2
SLOT_OF = [1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30]

# ---- 测试数据 ---------------------------------------------------------------

DOCS_DIR = "docs/"
NOTES_TXT = "notes.txt"
PHOTO_BMP = "photo.bmp"
CN_LONG = "项目验收清单文档.txt"          # 纯中文长名（9 汉字）
MIX_LONG = "报告v2-final-长名字.txt"      # 中英混排长名
ASCII_LONG = "readme-abcdefgh.txt"       # 纯 ASCII 长名
UNKNOWN = "data.bin"                      # 未知扩展名
EXTRA_TXT = "extra.txt"

SIX = [DOCS_DIR, NOTES_TXT, PHOTO_BMP, CN_LONG, MIX_LONG, ASCII_LONG]
EIGHT = [DOCS_DIR, NOTES_TXT, PHOTO_BMP, CN_LONG, MIX_LONG, UNKNOWN,
         ASCII_LONG, EXTRA_TXT]


def bmp16(color=(90, 140, 220)):
    w = h = 16
    stride = ((w * 3 + 3) & ~3)
    pix = bytearray()
    for _ in range(h):
        row = bytes(bytes((color[2], color[1], color[0])) * w)
        pix += row.ljust(stride, b"\x00")
    file_size = 54 + len(pix)
    hdr = bytearray(54)
    hdr[0:2] = b"BM"
    struct.pack_into("<I", hdr, 2, file_size)
    struct.pack_into("<I", hdr, 10, 54)
    struct.pack_into("<I", hdr, 14, 40)
    struct.pack_into("<i", hdr, 18, w)
    struct.pack_into("<i", hdr, 22, h)
    struct.pack_into("<H", hdr, 26, 1)
    struct.pack_into("<H", hdr, 28, 24)
    return bytes(hdr) + bytes(pix)


def ppm16(color=(120, 200, 120)):
    w = h = 16
    head = ("P6\n%d %d\n255\n" % (w, h)).encode()
    return head + bytes(bytes(color)) * (w * h)


FILE_DATA = {
    NOTES_TXT: b"R4 probe note line one\nline two\n",
    PHOTO_BMP: bmp16(),
    UNKNOWN: b"just some bytes\n",
    EXTRA_TXT: b"extra\n",
    CN_LONG: "中文长名文件内容：验收清单。\n".encode("utf-8"),
    MIX_LONG: "mixed content v2\n".encode("utf-8"),
    ASCII_LONG: "ascii long name body\n".encode("utf-8"),
}


# ---- 宿主侧 FAT32 / LFN 播种 ------------------------------------------------

def lfn_checksum(n83):
    s = 0
    for b in n83:
        s = ((0x80 if s & 1 else 0) + (s >> 1) + b) & 0xFF
    return s


def alias_for(name, idx):
    dot = name.rfind(".")
    stem = name[:dot] if dot > 0 else name
    ext = name[dot + 1:] if dot > 0 else ""
    a = [c.upper() for c in stem if c.isalnum() and ord(c) < 128]
    stem8 = "".join(a)[:6] or "FILE"
    short = "%s~%d" % (stem8, idx)
    ext3 = "".join(c.upper() for c in ext if c.isalnum())[:3]
    out = bytearray(b" " * 11)
    for i, ch in enumerate(short[:8]):
        out[i] = ord(ch)
    for i, ch in enumerate(ext3[:3]):
        out[8 + i] = ord(ch)
    return bytes(out)


def u16(name):
    return [ord(c) for c in name]


def lfn_entries(name, alias, attr, clus, size):
    ucs = u16(name)
    n = len(ucs)
    nent = max(1, (n + 12) // 13)
    check = lfn_checksum(alias)
    ents = []
    for k in range(nent):
        seq = nent - k
        e = bytearray(32)
        e[0] = (0x40 if seq == nent else 0) | seq
        e[11] = 0x0F
        e[13] = check
        base = (seq - 1) * 13
        for j in range(13):
            v = 0xFFFF
            if base + j < n:
                v = ucs[base + j]
            elif base + j == n:
                v = 0x0000
            slot = SLOT_OF[j]
            e[slot] = v & 0xFF
            e[slot + 1] = v >> 8
        ents.append(bytes(e))
    s = bytearray(32)
    s[0:11] = alias
    s[11] = attr
    struct.pack_into("<H", s, 20, clus >> 16)
    struct.pack_into("<H", s, 26, clus & 0xFFFF)
    struct.pack_into("<I", s, 28, size)
    ents.append(bytes(s))
    return ents


class Seeder(object):
    def __init__(self, path):
        self.img = bytearray(open(path, "rb").read())
        dbr = self.img[PART_START * 512:(PART_START + 1) * 512]
        self.spc = dbr[13]
        self.fatsz = struct.unpack_from("<H", dbr, 0x24)[0]
        self.data_start = PART_START + RESERVED + NUM_FATS * self.fatsz
        self.csz = self.spc * 512
        self.max_clus = self.fatsz * 512 // 4

    def fat_off(self, c):
        return (PART_START + RESERVED) * 512 + c * 4

    def clus_off(self, c):
        return (self.data_start + (c - 2) * self.spc) * 512

    def fat_get(self, c):
        return struct.unpack_from("<I", self.img, self.fat_off(c))[0] & 0x0FFFFFFF

    def fat_set(self, c, v):
        struct.pack_into("<I", self.img, self.fat_off(c), v)

    def alloc(self):
        for c in range(3, self.max_clus):
            if self.fat_get(c) == 0:
                self.fat_set(c, 0x0FFFFFFF)
                return c
        raise RuntimeError("no free cluster")

    def free_slot(self, clus):
        off = self.clus_off(clus)
        for i in range(0, self.csz, 32):
            if self.img[off + i] in (0x00, 0xE5):
                return off + i
        raise RuntimeError("dir cluster full %d" % clus)

    def write_file(self, data):
        csz = self.csz
        first = prev = None
        for i in range(0, max(len(data), 1), csz):
            c = self.alloc()
            if first is None:
                first = c
            else:
                self.fat_set(prev, c)
            prev = c
            chunk = data[i:i + csz].ljust(csz, b"\x00")
            self.img[self.clus_off(c):self.clus_off(c) + csz] = chunk
        return first

    def add_desktop_dir(self):
        dc = self.alloc()
        buf = bytearray(self.csz)
        # '.' / '..' 两条（父簇=根）
        def dot(n):
            d = bytearray(32)
            d[0] = ord(n[0]) if len(n) == 1 else 0x2E
            if len(n) == 2:
                d[1] = ord('.')
            d[11] = 0x10
            struct.pack_into("<H", d, 26, ROOT_CLUS & 0xFFFF)
            struct.pack_into("<H", d, 20, ROOT_CLUS >> 16)
            return bytes(d)
        buf[0:32] = dot(".")
        buf[32:64] = dot("..")
        self.img[self.clus_off(dc):self.clus_off(dc) + self.csz] = buf
        # 根目录追加 DESKTOP 条目（8.3，无 LFN）
        slot = self.free_slot(ROOT_CLUS)
        name83 = bytearray(b" " * 11)
        for i, ch in enumerate("DESKTOP"):
            name83[i] = ord(ch)
        e = bytearray(32)
        e[0:11] = name83
        e[11] = 0x10
        struct.pack_into("<H", e, 20, dc >> 16)
        struct.pack_into("<H", e, 26, dc & 0xFFFF)
        self.img[slot:slot + 32] = e
        return dc

    def add_entry(self, dc, name, idx, is_dir):
        attr = 0x10 if is_dir else 0x20
        if is_dir:
            clus = self.alloc()
            buf = bytearray(self.csz)
            d = bytearray(32)
            d[0] = ord('.')
            d[11] = 0x10
            struct.pack_into("<H", d, 26, clus & 0xFFFF)
            d2 = bytearray(32)
            d2[0] = ord('.'); d2[1] = ord('.')
            d2[11] = 0x10
            struct.pack_into("<H", d2, 26, dc & 0xFFFF)
            struct.pack_into("<H", d2, 20, dc >> 16)
            buf[0:32] = bytes(d); buf[32:64] = bytes(d2)
            self.img[self.clus_off(clus):self.clus_off(clus) + self.csz] = buf
            size = 0
        else:
            data = FILE_DATA.get(name, b"x\n")
            clus = self.write_file(data)
            size = len(data)
        alias = alias_for(name, idx)
        for ent in lfn_entries(name, alias, attr, clus, size):
            slot = self.free_slot(dc)
            self.img[slot:slot + 32] = ent

    def finalize(self, path):
        f1 = (PART_START + RESERVED) * 512
        f2 = f1 + self.fatsz * 512
        self.img[f2:f2 + self.fatsz * 512] = self.img[f1:f1 + self.fatsz * 512]
        open(path, "wb").write(self.img)


def build_disk(scenario):
    base = os.path.join(TMP, "base.img")
    if not os.path.exists(base):
        subprocess.run([sys.executable, MKDISK, "-o", base, "-m", "64"],
                       check=True, capture_output=True)
    out = os.path.join(TMP, "desk-%s.img" % scenario)
    subprocess.run(["cp", base, out], check=True)
    if scenario == "empty":
        return out
    names = SIX if scenario in ("six", "light") else EIGHT
    sd = Seeder(out)
    dc = sd.add_desktop_dir()
    for i, nm in enumerate(names):
        sd.add_entry(dc, nm.rstrip("/"), i + 1, nm.endswith("/"))
    sd.finalize(out)
    print("seeded %s: %d entries -> %s" % (scenario, len(names), out))
    return out


# ---- 探针动作 ---------------------------------------------------------------

def shoot(s, ppm_name, png_name):
    ppm = os.path.join(ART, ppm_name)
    png = os.path.join(ART, png_name)
    if os.path.exists(ppm):
        os.remove(ppm)
    cmd(s, "screendump " + ppm, 1.0)
    deadline = time.time() + 10
    while time.time() < deadline:
        if os.path.exists(ppm) and os.path.getsize(ppm) > 0:
            break
        time.sleep(0.3)
    from PIL import Image
    Image.open(ppm).convert("RGB").save(png)
    print("SHOT", png)
    return png


def mouse_diag(s):
    type_text(s, "mouse", 0.05)
    sendkey(s, "ret", 0.9)
    time.sleep(0.5)
    txt = read_screen(s)
    mm = re.findall(r"mouse: x=(\d+) y=(\d+) btn=(\d+)", txt)
    if not mm:
        return None
    m = mm[-1]
    return {"x": int(m[0]), "y": int(m[1])}


def click_icon(s, index):
    """列几何：x=GFX24_W-124=676，宽 104；item 高 52 + gap 8，pitch 60，y=64。"""
    cur = mouse_diag(s)
    tx = 728
    ty = 64 + index * 60 + 16
    if cur:
        cmd(s, "mouse_move %d %d" % (tx - cur["x"], ty - cur["y"]), 1.0)
    else:
        cmd(s, "mouse_move %d %d" % (tx, ty), 1.0)
    time.sleep(0.5)
    cmd(s, "mouse_button 1", 0.8)
    time.sleep(0.5)
    cmd(s, "mouse_button 0", 0.8)
    time.sleep(0.8)


def run_scenario(scenario):
    disk = build_disk(scenario)
    serial = os.path.join(ART, "serial-%s.log" % scenario)
    if os.path.exists(serial):
        os.remove(serial)
    qemu = find_qemu(None)
    light = scenario == "light"
    proc, s = boot(qemu, ISO, 4591 if scenario == "six" else 4592, disk=disk,
                    extra_args=["-smp", "1", "-cpu", "qemu32", "-vga", "vmware",
                                "-serial", "file:" + serial])
    try:
        force_text(s, settle=10)
        if not wait_for_prompt(s, timeout=90):
            print("FAIL: prompt never appeared (%s)" % scenario)
            return
        if light:
            type_text(s, "gui shot-light", 0.05)
        else:
            type_text(s, "gui", 0.05)
        sendkey(s, "ret", 1.0)
        time.sleep(6.0)
        if scenario == "six":
            shoot(s, "desk-r4.ppm", "desk-r4.png")
        elif scenario == "empty":
            shoot(s, "desk-empty-r4.ppm", "desk-empty-r4.png")
        elif light:
            shoot(s, "desk-light-r4.ppm", "desk-light-r4.png")
        else:
            shoot(s, "desk-eight-r4.ppm", "desk-eight-r4.png")
        # Esc 退回控制台，确认桌面可正常退出
        sendkey(s, "esc", 1.0)
        time.sleep(1.5)
        back = screen_prompt(s)
        print("  [%s] back-to-console after Esc: %s" % (scenario, back))
    finally:
        kill_qemu(proc)


def screen_prompt(s):
    import re
    time.sleep(0.5)
    txt = read_screen(s)
    return bool(re.search(r"catos:[^\n]*[\$#]", txt))


def main():
    which = sys.argv[1:] or ["six", "empty", "light", "eight"]
    for sc in which:
        print("== scenario %s ==" % sc)
        run_scenario(sc)


if __name__ == "__main__":
    main()
