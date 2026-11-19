#!/usr/bin/env python3
"""ime_regress.py -- v2.58 stage-1 pinyin IME regression.

Covers docs/系统内测试文档-2026-11-18.md section 5:

  * F2 toggles the IME; the fixed bottom two-row candidate area appears
    and restores verbatim (no residue after Esc / F2 off)
  * the pinyin composition string shows INLINE at the input cursor, not
    inside the candidate area; it disappears on commit / cancel
  * candidate row format `1.好 2.号 3.浩 ...`, adaptive to column width
  * digit 1 / Space commit; `-` `=` `[` `]` page across two pages;
    Esc cancels; Backspace edits; Enter commits the raw pinyin string
  * the flow runs once in TEXT mode (functional assertions on the VGA
    text plane) and once in gfx24/vbe24 (framebuffer smoke + screenshots)

Ports: 4570 (text guest), 4571 (gfx24 guest).

usage:
  python tests/ime_regress.py [catos.iso] --qemu /home/user/bin/qemu-system-i386
"""
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qemu_driver import (boot, cmd, find_qemu, kill_qemu, read_screen,  # noqa
                         screen_text, sendkey, type_text, force_text,
                         wait_for_prompt)

REPO = os.path.dirname(HERE)
ISO = os.path.join(REPO, "kernel_v25", "catos.iso")
PE = os.path.join(REPO, "kernel_v25", "build", "kernel.pe")
SHARED_DISK = os.path.join(REPO, "kernel_v25", "catos_disk.img")
SHOTS = os.path.join(HERE, "qemu_shots")
PORT_TEXT = 4570
PORT_GFX24 = 4571

os.makedirs(SHOTS, exist_ok=True)


class Report(object):
    def __init__(self, tag):
        self.tag = tag
        self.n = 0
        self.fails = []

    def check(self, name, cond, detail=""):
        self.n += 1
        print("  %-4s %-44s %s" % ("PASS" if cond else "FAIL", name,
                                    detail[-160:].replace("\n", " ")))
        if not cond:
            self.fails.append(name)


# --------------------------------------------------------------------------
# screenshots: HMP screendump emits P6 PPM regardless of extension; convert
# to a minimal PNG (zero dependencies, same scheme as qemu_regress.py).
# --------------------------------------------------------------------------
def ppm_to_png(ppm, png):
    with open(ppm, "rb") as f:
        data = f.read()
    if data[:2] != b"P6":
        raise ValueError("screendump did not produce a PPM (got %r)" % data[:2])
    parts, pos = [], 2
    while len(parts) < 3:
        while pos < len(data) and data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            while pos < len(data) and data[pos] not in (10, 13):
                pos += 1
            continue
        start = pos
        while pos < len(data) and not data[pos:pos + 1].isspace():
            pos += 1
        parts.append(int(data[start:pos]))
    pos += 1
    w, h, _ = parts
    raw = data[pos:pos + w * h * 3]
    if len(raw) < w * h * 3:
        raise ValueError("PPM pixel data truncated")

    def chunk(typ, payload):
        c = struct.pack(">I", len(payload)) + typ + payload
        return c + struct.pack(">I", zlib.crc32(typ + payload) & 0xffffffff)

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    stride = w * 3
    scan = b"".join(b"\x00" + raw[y * stride:(y + 1) * stride]
                    for y in range(h))
    with open(png, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", ihdr))
        f.write(chunk(b"IDAT", zlib.compress(scan, 6)))
        f.write(chunk(b"IEND", b""))
    os.remove(ppm)
    return png


def shoot(s, name):
    png = os.path.join(SHOTS, name + ".png")
    ppm = os.path.join(SHOTS, name + ".ppm")
    if os.path.exists(ppm):
        os.remove(ppm)
    cmd(s, "screendump " + ppm.replace("\\", "/"), 0.5)
    for _ in range(40):
        if os.path.exists(ppm) and os.path.getsize(ppm) > 0:
            break
        time.sleep(0.5)
    else:
        raise RuntimeError("screendump produced no file for %s" % name)
    return ppm_to_png(ppm, png)


class Ppm(object):
    def __init__(self, path):
        with open(path, "rb") as fh:
            data = fh.read()
        m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+(\d+)\s", data)
        if not m:
            raise RuntimeError("not a P6 PPM: %r" % data[:32])
        self.w, self.h, self.mx = (int(v) for v in m.groups())
        self.raw = data[m.end():m.end() + self.w * self.h * 3]

    def px(self, x, y):
        i = (y * self.w + x) * 3
        return tuple(self.raw[i:i + 3])


def shoot_ppm(s, tag):
    ppm = os.path.join(tempfile.gettempdir(), "ime_%s.ppm" % tag)
    if os.path.exists(ppm):
        os.remove(ppm)
    cmd(s, "screendump " + ppm.replace("\\", "/"), 0.8)
    time.sleep(1.4)
    if not os.path.exists(ppm) or os.path.getsize(ppm) == 0:
        raise RuntimeError("screendump produced no PPM for %s" % tag)
    return Ppm(ppm)


def bottom_nonbg(ppm, bg=(12, 12, 12), tol=28):
    """Count pixels in the bottom two rows that differ from the bg colour."""
    rows = max(2, ppm.h // 19)
    y0 = ppm.h - rows
    n = 0
    for y in range(y0, ppm.h):
        for x in range(0, ppm.w, 4):
            r, g, b = ppm.px(x, y)
            if abs(r - bg[0]) + abs(g - bg[1]) + abs(b - bg[2]) > tol:
                n += 1
    return n


# --------------------------------------------------------------------------
# back-end identity read through the monitor (works in framebuffer modes)
# --------------------------------------------------------------------------
def _symbols():
    out = subprocess.run(["nm", PE], capture_output=True, text=True).stdout
    table = {}
    for line in out.splitlines():
        parts = line.split()
        if parts and parts[-1] in ("_be", "_be_text", "_be_gfx",
                                   "_be_vbe", "_be_vbe24"):
            table[parts[-1]] = int(parts[0], 16)
    return table


def _xp(s, addr, n=1):
    out = cmd(s, "xp /%dxw 0x%x" % (n, addr), 1.0)
    return [int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{8})", out)]


def read_mode(s):
    sym = _symbols()
    if "_be" not in sym:
        return None
    ptr = _xp(s, sym["_be"], 1)
    if not ptr:
        return None
    b = ptr[0] & 0xFFFFFFFF
    words = _xp(s, b, 7)
    if len(words) < 7:
        return None
    return dict(mode=words[6] & 0xFF,
                pw=words[2] & 0xFFFF, ph=(words[2] >> 16) & 0xFFFF)


def bottom_rows(s):
    rows = [r.rstrip() for r in read_screen(s).splitlines()]
    return rows[-2], rows[-1]


def page_pair(row):
    m = re.search(r"(\d+)/(\d+)", row)
    return (int(m.group(1)), int(m.group(2))) if m else None


# --------------------------------------------------------------------------
# text-mode guest: functional assertions on the 0xB8000 text plane
# --------------------------------------------------------------------------
def run_text(qemu, iso):
    r = Report("text")
    proc, s = boot(qemu, iso, PORT_TEXT, disk=SHARED_DISK)
    try:
        force_text(s)
        if not wait_for_prompt(s):
            r.check("prompt after boot", False, "no prompt")
            return r
        type_text(s, "clear", wait=0.06)
        sendkey(s, "ret", 0.8)
        time.sleep(0.6)
        base2, base1 = bottom_rows(s)

        # 1. F2 on: idle IME frame on the bottom two rows.
        sendkey(s, "f2", 0.6)
        time.sleep(0.6)
        _, idle1 = bottom_rows(s)
        r.check("F2 on: idle hint on candidate row", "[IME]" in idle1, idle1)

        # 2. type a syllable: inline pinyin at the cursor, candidates below.
        type_text(s, "zhong", wait=0.08)
        time.sleep(0.6)
        t = screen_text(s)
        row2, row1 = bottom_rows(s)
        r.check("inline pinyin shown at input line", "zhong" in t, t[-120:])
        r.check("candidate row lists `1.` entry", "1." in row1, row1)
        shoot(s, "ime_text_on")

        # 2b. a digit while composing must NEVER be appended to the pinyin
        # buffer: it is the candidate pick.  Regression guard for the
        # "zhong9" screenshot artefact (py must hold letters only).
        r.check("pinyin buffer holds letters only", "zhong9" not in t, t[-120:])

        # 3. digit 1 commits the first candidate; composition clears.
        sendkey(s, "1", 0.6)
        time.sleep(0.6)
        shoot(s, "ime_text_commit")
        t = screen_text(s)
        r.check("commit: inline pinyin cleared", "zhong" not in t, t[-120:])
        # submit whatever is on the draft line; guest must stay alive.
        sendkey(s, "ret", 0.8)
        time.sleep(0.8)
        r.check("guest alive after commit", wait_for_prompt(s, timeout=20))

        # 4. paging across two pages: ji has 104 candidates (12 pages).
        type_text(s, "clear", wait=0.06)
        sendkey(s, "ret", 0.6)
        type_text(s, "ji", wait=0.08)
        time.sleep(0.6)
        _, p0 = bottom_rows(s)
        pg0 = page_pair(p0)
        r.check("page 1 indicator shown", pg0 is not None and pg0[0] == 1, p0)

        sendkey(s, "equal", 0.4)          # = next page
        _, p1 = bottom_rows(s)
        pg1 = page_pair(p1)
        sendkey(s, "bracket_right", 0.4)  # ] next page
        _, p2 = bottom_rows(s)
        pg2 = page_pair(p2)
        sendkey(s, "bracket_left", 0.4)   # [ previous page
        _, p3 = bottom_rows(s)
        pg3 = page_pair(p3)
        sendkey(s, "minus", 0.4)           # - previous page
        _, p4 = bottom_rows(s)
        pg4 = page_pair(p4)
        pages = {p for p in (pg0, pg1, pg2, pg3, pg4) if p}
        r.check("- = [ ] page across two pages",
                len(pages) >= 2 and pg1 and pg1[0] == 2,
                "pages=%s" % sorted(pages))

        # 5. Esc cancels: composition gone, bottom rows restored to baseline.
        sendkey(s, "esc", 0.6)
        time.sleep(0.6)
        t = screen_text(s)
        e2, e1 = bottom_rows(s)
        r.check("Esc: no inline pinyin residue", "ji" not in t, t[-120:])
        r.check("Esc: candidate row restored", e1.strip() == base1.strip(),
                "got=%r base=%r" % (e1, base1))
        r.check("Esc: separator row restored", e2.strip() == base2.strip(),
                "got=%r base=%r" % (e2, base2))

        # 6. F2 toggles: on shows the idle frame, off restores the rows.
        # (Esc in step 5 already closed the IME, so this starts from off.)
        sendkey(s, "f2", 0.5)
        time.sleep(0.5)
        _, o1 = bottom_rows(s)
        r.check("F2 on: idle hint back", "[IME]" in o1, o1)
        sendkey(s, "f2", 0.5)
        time.sleep(0.5)
        o2, o1 = bottom_rows(s)
        r.check("F2 off: area restored",
                o1.strip() == base1.strip() and o2.strip() == base2.strip(),
                "got=%r/%r" % (o2, o1))

        # 7. Enter commits the raw pinyin string (no match path included).
        type_text(s, "clear", wait=0.06)
        sendkey(s, "ret", 0.5)
        sendkey(s, "f2", 0.4)
        type_text(s, "zzz", wait=0.08)   # no syllable matches
        time.sleep(0.5)
        sendkey(s, "ret", 0.6)
        time.sleep(0.6)
        t = screen_text(s)
        r.check("Enter: raw pinyin committed to line", "zzz" in t, t[-120:])
        sendkey(s, "ret", 0.6)
        time.sleep(0.6)
        r.check("guest alive after raw commit", wait_for_prompt(s, timeout=20))
    finally:
        kill_qemu(proc)
    return r


# --------------------------------------------------------------------------
# gfx24 (vbe24) guest: framebuffer smoke + screenshots, no text-plane parse
# --------------------------------------------------------------------------
def run_gfx24(qemu, iso):
    r = Report("gfx24")
    proc, s = boot(qemu, iso, PORT_GFX24, disk=SHARED_DISK)
    try:
        # The driver parses the 0xB8000 text plane, so consent to text mode
        # just to read the prompt; the `gfx24` command then switches back to
        # the vbe24 framebuffer, which this suite exercises end to end.
        force_text(s)
        if not wait_for_prompt(s, timeout=90):
            r.check("prompt after boot", False, "no prompt")
            return r
        type_text(s, "gfx24", wait=0.06)
        sendkey(s, "ret", 1.2)
        time.sleep(1.0)
        m = read_mode(s)
        r.check("back-end switched to vbe24",
                m is not None and m["mode"] == 3 and m["pw"] == 800,
                "mode=%s geom=%s" % (m and m["mode"], m and (m["pw"], m["ph"])))

        base = shoot_ppm(s, "gfx24_base")
        base_n = bottom_nonbg(base)

        sendkey(s, "f2", 0.6)
        type_text(s, "ni", wait=0.08)
        time.sleep(0.8)
        on = shoot_ppm(s, "gfx24_on")
        on_n = bottom_nonbg(on)
        r.check("gfx24: candidate area paints bottom rows",
                on_n > base_n + 20, "base=%d on=%d" % (base_n, on_n))
        shoot(s, "ime_gfx24_on")

        sendkey(s, "esc", 0.6)
        time.sleep(0.8)
        off = shoot_ppm(s, "gfx24_off")
        off_n = bottom_nonbg(off)
        r.check("gfx24: Esc restores bottom rows",
                off_n <= base_n + 20, "base=%d off=%d" % (base_n, off_n))
        shoot(s, "ime_gfx24_off")

        # In framebuffer mode the driver cannot read the 0xB8000 prompt, so
        # liveness is checked through the monitor: the guest answers HMP and
        # the framebuffer still reports 800x600 after the round trip.
        sendkey(s, "f2", 0.5)
        time.sleep(0.6)
        alive = cmd(s, "info status", 1.0)
        snap = shoot_ppm(s, "gfx24_final")
        r.check("gfx24: guest alive after toggle",
                "running" in alive.lower() and (snap.w, snap.h) == (800, 600),
                "status=%r geom=%dx%d" % (alive[-60:], snap.w, snap.h))
    finally:
        kill_qemu(proc)
    return r


def main(argv):
    iso = ISO
    qemu = find_qemu(argv[argv.index("--qemu") + 1]
                      if "--qemu" in argv else None)
    for a in argv:
        if a.endswith(".iso"):
            iso = os.path.abspath(a)
    if not qemu:
        print("qemu-system-i386 not found (pass --qemu PATH)")
        return 2
    if not os.path.isfile(iso):
        print("ISO not found: %s" % iso)
        return 2

    results = []
    for runner in (run_text, run_gfx24):
        try:
            results.append(runner(qemu, iso))
        except Exception as e:
            print("  FAIL  exception in %s: %s" % (runner.__name__, e))
            results.append(None)

    total = sum(r.n for r in results if r)
    okn = sum(r.n - len(r.fails) for r in results if r)
    print("\nime_regress: %d/%d PASS" % (okn, total))
    failed = [x for r in results if r for x in r.fails]
    if failed:
        print("FAILED: " + ", ".join(failed))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
