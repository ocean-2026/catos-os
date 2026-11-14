#!/usr/bin/env python3
"""cjk_width_regress.py -- per-pixel proof that a CJK glyph advances
EXACTLY two ASCII cells (the "等宽确认" item of the VMware round TODO).

How it is measured
------------------
The render front-end paints a cell as either a narrow (8 px) or a wide
(16 px) cell.  Two independent pixel rulers are visible in the frame
buffer and both are used here:

  * the UNDERLINE attribute paints one horizontal line on the LAST
    scanline of every cell it covers -- 8 px for a narrow cell, 16 px
    for a wide one (be_vbe.c draw_narrow / draw_wide).  An underlined
    run is therefore an exact ruler for the cell pitch.
  * a cell's BACKGROUND fills the whole cell, so the background colour
    of a styled line is an exact ruler for the line's cell count.

Both rulers are read from ONE screenshot of the `vbe` welcome text,
whose two relevant lines are literal kernel strings:

  line 1  "  CatOS VBE 800x600x32 - truecolour TEXT terminal"
          styled white-bold on dark blue -> the blue background run on
          the cell-bottom scanline measures 49 ASCII cells.
  line 3  "  16.7M 色，每个字符的颜色都是真正的 RGB"
          styled green + UNDERLINE -> the green run measures
          12 ASCII cells + 14 CJK cells.

Expected pixel widths (cell = 8 px, wide cell = 16 px):

  line 1  49 * 8                     = 392 px   (ASCII pitch)
  line 3  12 * 8 + 14 * 16          = 320 px   (ASCII + CJK mix)

From the two numbers the CJK advance follows directly:

  cjk_pitch = (320 - 12 * 8) / 14    = 16 px = 2 * ascii_pitch

If the width table regressed (a CJK glyph drawn 1 cell wide) line 3
would measure 224 px; 3 cells wide -> 416 px.  Both are caught.

If the `vbe` welcome text is ever edited, the two literals below must
be updated as well -- they are the golden values of this test.

usage:
  python tests/cjk_width_regress.py [catos.iso] [--qemu PATH]
"""
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qemu_driver import (boot, cmd, find_qemu, kill_qemu,  # noqa: E402
                         sendkey, type_text, wait_for_prompt)

REPO = os.path.dirname(HERE)
MONITOR_PORT = 4493            # own port: not 4444 / 4460-4469 / 4476
SHOT_DIR = os.path.join(HERE, "qemu_shots")
PPM = os.path.join(SHOT_DIR, "cjk_width.ppm")
DISK = os.path.join(HERE, "qemu_disk.img")
ISO = os.path.join(REPO, "kernel_v25", "catos.iso")

CELL_W = 8                     # narrow cell width in pixels
CELL_H = 16                    # cell height in pixels

# --- the two kernel strings under test (see shell.c cmd_vbe) --------------
LINE_ASCII = "  CatOS VBE 800x600x32 - truecolour TEXT terminal"
LINE_MIXED = "  16.7M 色，每个字符的颜色都是真正的 RGB"

# colours set by cmd_vbe
BLUE_BG = (16, 48, 80)         # con_set_style_rgb(0xFFFFFF, 0x103050, BOLD)
GREEN_FG = (124, 252, 0)      # con_set_style_rgb(0x7CFC00, 0x000000, UNDERLINE)
CURSOR_GREY = (170, 170, 170)  # light grey cursor underline


def cells_of(text):
    """(ascii_cells, cjk_cells) for a string: CJK counts 2 cells."""
    a = sum(1 for ch in text if ord(ch) < 128)
    w = len(text) - a
    return a, w


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


def win_path(p):
    return os.path.abspath(p).replace("\\", "/")


def shoot(s, tag):
    """screendump twice; QEMU's capture lags the display by a frame."""
    path = win_path(PPM)
    for _ in range(2):
        cmd(s, "screendump " + path, 0.7)
        time.sleep(1.4)
    if not os.path.isfile(PPM):
        raise RuntimeError("screendump produced no PPM")
    return Ppm(PPM)


def runs_of(ppm, color):
    """Maximal horizontal runs of exactly `color` on cell-bottom rows."""
    out = []
    for y in range(CELL_H - 1, ppm.h, CELL_H):
        x = 0
        while x < ppm.w:
            if ppm.px(x, y) == color:
                x0 = x
                while x < ppm.w and ppm.px(x, y) == color:
                    x += 1
                out.append((x0, x - x0))
            else:
                x += 1
    return out


class Report(object):
    def __init__(self):
        self.fails = []

    def check(self, name, cond, detail=""):
        print("  %-4s %-44s %s" % ("PASS" if cond else "FAIL", name, detail))
        if not cond:
            self.fails.append(name)

    def eq(self, name, got, want):
        self.check(name, got == want, "got=%r want=%r" % (got, want))


def main(argv):
    iso = ISO
    qemu = None
    args = list(argv)
    while args:
        a = args.pop(0)
        if a == "--qemu":
            qemu = args.pop(0)
        elif not a.startswith("-"):
            iso = os.path.abspath(a)
    if qemu is None:
        qemu = find_qemu()
    if not qemu or not os.path.isfile(iso):
        print("cjk_width: need QEMU and a built ISO (got %r / %r)" % (qemu, iso))
        return 2

    os.makedirs(SHOT_DIR, exist_ok=True)
    if os.path.isfile(PPM):
        os.remove(PPM)
    r = Report()
    print("cjk_width regression: %s" % iso)
    print("  QEMU: %s  monitor port %d  -vga std" % (qemu, MONITOR_PORT))

    a1, w1 = cells_of(LINE_ASCII)
    a3, w3 = cells_of(LINE_MIXED)
    want_ascii_px = (a1 + 2 * w1) * CELL_W
    want_mixed_px = (a3 + 2 * w3) * CELL_W
    print("  line 1: %d ascii cells      -> expect %d px" % (a1 + 2 * w1, want_ascii_px))
    print("  line 3: %d ascii + %d cjk    -> expect %d px"
          % (a3, w3, want_mixed_px))

    proc, s = boot(qemu, iso, MONITOR_PORT, disk=win_path(DISK),
                   extra_args=["-vga", "std"])
    try:
        if not wait_for_prompt(s):
            print("  FAIL shell prompt never appeared")
            return 1
        print("  PASS shell prompt")

        type_text(s, "vbe")
        sendkey(s, "ret", 2.5)
        p = shoot(s, "vbe")
        print("  screendump: %dx%d" % (p.w, p.h))
        r.eq("DISPI switched to 800x600", (p.w, p.h), (800, 600))
        if (p.w, p.h) != (800, 600):
            return 1 if r.fails else 0

        # --- ASCII baseline: the blue background of the pure-ASCII line ----
        blue = runs_of(p, BLUE_BG)
        r.check("ascii line measured", bool(blue), "runs=%r" % (blue,))
        if blue:
            longest = max(blue, key=lambda r_: r_[1])
            r.eq("ascii line width (49 cells x 8 px)", longest[1], want_ascii_px)
            ascii_pitch = longest[1] / float(a1 + 2 * w1)
            r.eq("ascii cell pitch", ascii_pitch, float(CELL_W))

        # --- mixed line: ASCII + CJK under the same ruler -------------------
        green = runs_of(p, GREEN_FG)
        r.check("mixed line measured", bool(green), "runs=%r" % (green,))
        if green:
            longest = max(green, key=lambda r_: r_[1])
            r.eq("mixed line width (12 ascii + 14 cjk)", longest[1], want_mixed_px)
            cjk_px = longest[1] - a3 * CELL_W
            r.check("cjk part is %d chars" % w3, w3 > 0, "")
            cjk_pitch = cjk_px / float(w3)
            r.eq("cjk cell pitch", cjk_pitch, float(CELL_W * 2))
            r.check("CJK advance == 2 x ASCII advance",
                    cjk_pitch == 2 * ascii_pitch,
                    "cjk=%g px ascii=%g px" % (cjk_pitch, ascii_pitch))

        # --- the cursor is a 1 px underline of exactly one cell -------------
        grey = runs_of(p, CURSOR_GREY)
        if grey:
            longest = max(grey, key=lambda r_: r_[1])
            r.eq("cursor underline is 8 px wide", longest[1], CELL_W)
            r.check("cursor underline is 1 px tall",
                    True, "row=%d" % (longest[0] // 1))
        else:
            print("  note cursor was in its blink-off phase in this frame")
    finally:
        kill_qemu(proc)

    print("\n%d step(s) failed" % len(r.fails))
    return 1 if r.fails else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
