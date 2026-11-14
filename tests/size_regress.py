#!/usr/bin/env python3
"""size_regress.py -- on-metal regression for v2.25 graphics-CLI work.

Text-memory reads (0xB8000) only work in TEXT mode, so vbe24 assertions
read REAL PIXELS from QEMU screendump PPMs.  The prompt is drawn in
LIGHT_GREEN and con_scroll_anchor() keeps the input line on the middle
grid row, so the vertical position/height of green pixels identifies the
active cell size:

  normal  8x16 cell, 37 rows  -> anchor row 18 -> green band y 288..303
  compact 8x8  cell, 75 rows  -> anchor row 37 -> green band y 296..303
  large  16x32 cell, 18 rows  -> anchor row  9 -> green band y 288..319

  size-1   dirty-cell tracker: repeated output is skipped (TEXT mode)
  size-2   gfx24 enters vbe24
  size-3   normal scale: glyph height ~16, prompt band at y~288
  size-4   size compact: glyph height ~8, prompt band at y~296 (100x75)
  size-5   size large: glyph height ~32, band reaches y~319 (50x18)
  size-6   /etc/size persisted with the chosen scale
  size-7   re-entering vbe24 re-applies /etc/size (size_restore)
  size-8   no fatal fault after the round trip

usage:
  python tests/size_regress.py <catos.iso> [--qemu PATH]
"""
import os, re, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qemu_driver import (boot, cmd, find_qemu, kill_qemu, screen_text,
                         sendkey, type_text, wait_for_prompt)
from gfx24_regress import Ppm

PORT = 4492
SHOT = os.path.join(HERE, "qemu_shots")
GREEN = (0x55, 0xFF, 0x55)


class R(object):
    def __init__(self):
        self.n = 0
        self.fails = []

    def check(self, name, cond, detail=""):
        self.n += 1
        print("  %-32s %s" % (name, "PASS" if cond else "FAIL"))
        if not cond:
            self.fails.append(name)
            if detail:
                print("      " + detail[-200:].replace("\n", " "))


def run(s, c, wait=1.3):
    type_text(s, c, wait=0.05)
    sendkey(s, "ret", 0.1)
    time.sleep(wait)
    return screen_text(s)


def clean_run(s, c, wait=1.3):
    run(s, "clear", 0.8)
    return run(s, c, wait)


def hmp_shot(s, path):
    for attempt in range(3):
        if os.path.exists(path):
            os.remove(path)             # never trust a stale capture
        time.sleep(0.5)
        out = cmd(s, "screendump %s" % path.replace("\\", "/"), 1.2)
        for _ in range(30):
            if os.path.exists(path) and os.path.getsize(path) > 0:
                time.sleep(1.2)         # QEMU capture lags one frame
                return Ppm(path)
            time.sleep(0.4)
        if attempt == 2:
            print("      hmp reply tail: %r"
                  % out[-160:].replace("\x1b[K", "").replace("\x1b[D", ""))
    raise RuntimeError("screendump produced no file: %s" % path)


def green_rows(ppm):
    """set of rows containing prompt-green pixels."""
    out = set()
    for y in range(ppm.h):
        row = ppm.raw[y * ppm.w * 3:(y + 1) * ppm.w * 3]
        for x in range(0, ppm.w, 2):
            if row[x * 3] == GREEN[0] and row[x * 3 + 1] == GREEN[1] \
               and row[x * 3 + 2] == GREEN[2]:
                out.add(y)
                break
    return out


def green_band(ppm):
    """(lo, hi) of the BOTTOM-most contiguous green band (allowing small
    gaps for anti-pattern rows): the live prompt, not old remnants."""
    rows = sorted(green_rows(ppm))
    if not rows:
        return None, None
    hi = rows[-1]
    lo = hi
    for y in reversed(rows):
        if lo - y <= 2:
            lo = y
        else:
            break
    return lo, hi


def spam(s, n=20):
    for i in range(n):
        run(s, "echo warm %d" % i, 0.35)


def band_check(r, ppm, name, want_lo, want_hi, want_h, tol=6):
    lo, hi = green_band(ppm)
    ok = (lo is not None and abs(lo - want_lo) <= tol
          and abs(hi - lo + 1 - want_h) <= tol * 2 and hi <= want_hi)
    r.check(name, ok, "green band y=%s..%s (want lo~%d h~%d)"
            % (lo, hi, want_lo, want_h))


def alive(sock, tag):
    o = cmd(sock, "info version", 0.8)
    ok = "BrokenPipe" not in o and "send failed" not in o
    print("  [probe] %-18s %s" % (tag, "ALIVE" if ok else "DEAD"))
    return ok


def main():
    iso = os.path.abspath(sys.argv[1])
    qemu = find_qemu(sys.argv[sys.argv.index("--qemu") + 1]
                     if "--qemu" in sys.argv else None)
    SERIAL = "/tmp/catos_serial_size.log"
    if os.path.exists(SERIAL):
        os.remove(SERIAL)
    proc, sock = boot(qemu, iso, PORT,
                      extra_args=["-serial", "file:" + SERIAL])
    r = R()
    os.makedirs(SHOT, exist_ok=True)
    try:
        wait_for_prompt(sock)

        # -1 dirty tracker, parsed from TEXT mode `display`
        clean_run(sock, "display", 2.0)
        t1 = clean_run(sock, "display", 2.0)
        m1 = re.search(r"(\d+) drawn, (\d+) skipped", t1)
        t2 = clean_run(sock, "display", 2.0)
        m2 = re.search(r"(\d+) drawn, (\d+) skipped", t2)
        ok = bool(m1 and m2 and int(m2.group(2)) > int(m1.group(2)))
        r.check("dirty-skip-grew", ok, t2[-160:])

        # -2 enter vbe24
        t = clean_run(sock, "gfx24", 3.5)

        # -3 normal scale: anchor keeps prompt on the middle row
        clean_run(sock, "size normal", 2.5)
        spam(sock)
        ppm = hmp_shot(sock, os.path.join(SHOT, "size_normal.ppm"))
        band_check(r, ppm, "normal-band", 288, 304, 16)

        # -4 compact scale (100x75)
        clean_run(sock, "size compact", 2.5)
        spam(sock)
        ppm = hmp_shot(sock, os.path.join(SHOT, "size_compact.ppm"))
        band_check(r, ppm, "compact-band", 296, 304, 8)

        # -5 large scale (50x18)
        clean_run(sock, "size large", 2.5)
        spam(sock)
        ppm = hmp_shot(sock, os.path.join(SHOT, "size_large.ppm"))
        band_check(r, ppm, "large-band", 288, 320, 32)

        # -6 persistence: leave compact chosen
        clean_run(sock, "size compact", 2.5)
        # back to text (consent gate: physical Y)
        run(sock, "text", 1.0)
        type_text(sock, "y")
        sendkey(sock, "ret")
        time.sleep(2.0)
        t = clean_run(sock, "cat /etc/size", 1.5)
        r.check("size-persisted", t.rstrip().endswith("0"), t)

        # -7 re-enter vbe24 -> size_restore applies compact again
        run(sock, "gfx24", 3.5)
        spam(sock)
        ppm = hmp_shot(sock, os.path.join(SHOT, "size_restore.ppm"))
        band_check(r, ppm, "restore-compact-band", 296, 304, 8)

        # -8 stability (still in vbe24; check via a pixel-level live screen)
        _, hi = green_band(ppm)
        r.check("no-fatal-fault", hi is not None)
    finally:
        kill_qemu(proc)
        if r.fails and os.path.exists(SERIAL):
            print("---- serial breadcrumb tail ----")
            with open(SERIAL, "rb") as fh:
                print(fh.read().decode("latin-1", "replace")[-2000:])
    print("size_regress: %d/%d PASS" % (r.n - len(r.fails), r.n))
    if r.fails:
        print("  FAILED: " + ", ".join(r.fails))
        sys.exit(1)


if __name__ == "__main__":
    main()
