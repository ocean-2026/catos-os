#!/usr/bin/env python3
"""gfx24_regress.py -- pixel-exact on-metal acceptance for v2.12 24bpp.

Boots CatOS in QEMU with `-vga std` (Bochs dispi), then checks the whole
24-bit path by reading REAL PIXELS out of QEMU's `screendump`:

  step 1  `/bin/gfx24.ce` is installed in the built-in image (ls /bin)
  step 2  shell `gfx24` switches to 800x600x24 and paints the fixed demo:
          four pure colour blocks, a 256-step red/blue ramp, eight bars
  step 3  ring3 `run /bin/gfx24.ce` draws through the three NEW syscalls
          (gfx_info / gfx_rect / gfx_blit) with a USER-space bitmap
  step 4  the demo pixels must still be intact (no clear / no corruption)

A driver that merely sets DISPI_BPP=24 but writes 32bpp pixels, or one
that gets the BGR byte order wrong, fails here even though the screen
would "look like something".

QEMU's screendump is asynchronous and its capture lags the display state
by one or two frames, so every shot is taken twice and only the LAST PPM
is parsed.  Kill by PID only -- never taskkill /IM (shared machine).

usage:
  python tests/gfx24_regress.py [catos.iso] [--qemu PATH] [--keep]
"""
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qemu_driver import (boot, cmd, find_qemu, kill_qemu,  # noqa: E402
                         screen_text, sendkey, type_text,
                         wait_for_prompt)

REPO = os.path.dirname(HERE)
MONITOR_PORT = 4462                 # own port: not 4444 / 4460 / 4461
SHOT_DIR = os.path.join(HERE, "qemu_shots")
PPM = os.path.join(SHOT_DIR, "gfx24.ppm")
DISK = os.path.join(HERE, "qemu_disk_gfx24.img")
ISO = os.path.join(REPO, "kernel_v25", "catos.iso")


def win_path(p):
    """QEMU is native Windows and cannot open MSYS /tmp paths."""
    return os.path.abspath(p).replace("\\", "/")


def drain(s):
    """Drop stale HMP echo (sendkey echoes arrive asynchronously)."""
    s.setblocking(False)
    try:
        while True:
            if not s.recv(65536):
                break
    except BlockingIOError:
        pass
    finally:
        s.setblocking(True)


def hmp(s, c, wait=1.0):
    drain(s)
    return cmd(s, c, wait)


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

    def __str__(self):
        return "%dx%d max=%d" % (self.w, self.h, self.mx)


def shoot(s, tag):
    """screendump twice; QEMU captures lag, the last frame is authoritative."""
    path = win_path(PPM)
    for _ in range(2):
        hmp(s, "screendump " + path, 0.8)
        time.sleep(1.6)
    if not os.path.isfile(PPM):
        raise RuntimeError("screendump produced no PPM")
    return Ppm(PPM)


class Report(object):
    def __init__(self):
        self.fails = []

    def check(self, name, cond, detail=""):
        print("  %-4s %-46s %s" % ("PASS" if cond else "FAIL", name, detail))
        if not cond:
            self.fails.append(name)

    def eq_px(self, ppm, x, y, want, name):
        got = ppm.px(x, y)
        self.check(name, got == want,
                   "(%d,%d) got=%s want=%s" % (x, y, got, want))


def main(argv):
    iso = ISO
    qemu = None
    keep = False
    args = list(argv)
    while args:
        a = args.pop(0)
        if a == "--qemu":
            qemu = args.pop(0)
        elif a == "--keep":
            keep = True
        elif not a.startswith("-"):
            iso = os.path.abspath(a)
    if qemu is None:
        qemu = find_qemu()
    if not qemu or not os.path.isfile(iso):
        print("gfx24: need QEMU and a built ISO (got %r / %r)" % (qemu, iso))
        return 2

    os.makedirs(SHOT_DIR, exist_ok=True)
    if os.path.isfile(PPM):
        os.remove(PPM)
    r = Report()
    print("gfx24 regression: %s" % iso)
    print("  QEMU: %s  monitor port %d  -vga std" % (qemu, MONITOR_PORT))

    # The built-in CE apps are installed onto the FAT volume by `format`,
    # and that only happens when an IDE disk is attached at boot.
    if not os.path.exists(DISK):
        with open(DISK, "wb") as fh:
            fh.truncate(64 * 1024 * 1024)     # sparse 64 MiB
    proc, s = boot(qemu, iso, MONITOR_PORT, disk=win_path(DISK),
                   extra_args=["-vga", "std"])
    try:
        if not wait_for_prompt(s):
            print("  FAIL shell prompt never appeared")
            return 1
        print("  PASS shell prompt")

        # ---- step 0: format installs the built-in apps --------------------
        type_text(s, "format")
        sendkey(s, "ret", 1.0)
        sendkey(s, "y", 0.4)
        sendkey(s, "ret", 15.0)
        r.check("format + builtin install", wait_for_prompt(s, timeout=90.0))

        # ---- step 1: image contains the new CE app ------------------------
        type_text(s, "clear\n")
        time.sleep(0.6)
        type_text(s, "ls /bin\n")
        time.sleep(2.5)
        txt = screen_text(s).lower()
        r.check("/bin/gfx24.ce installed", "gfx24.ce" in txt,
                "" if "gfx24.ce" in txt else txt[-160:].replace("\n", " "))

        # ---- step 2: kernel-side demo picture -----------------------------
        type_text(s, "gfx24\n")
        time.sleep(3.0)
        p = shoot(s, "demo")
        print("  screendump: %s" % p)
        r.check("DISPI switched to 800x600", (p.w, p.h) == (800, 600),
                "got %dx%d" % (p.w, p.h))
        if (p.w, p.h) != (800, 600):
            print("  (cannot sample pixels at the wrong resolution)")
            return 1 if r.fails else 0
        r.eq_px(p, 150, 170, (255, 0, 0), "red block (R channel)")
        r.eq_px(p, 300, 170, (0, 255, 0), "green block (G channel)")
        r.eq_px(p, 150, 320, (0, 0, 255), "blue block (B channel)")
        r.eq_px(p, 300, 320, (255, 255, 255), "white block (all channels)")
        r.eq_px(p, 450, 200, (12, 12, 12), "24-bit backdrop 0x0C0C0C (v2.37 theme)")
        r.eq_px(p, 400, 440, (127, 0, 128), "256-step gradient midpoint")
        r.eq_px(p, 50, 520, (0, 0, 0), "bar 0 black")
        r.eq_px(p, 750, 520, (255, 255, 255), "bar 7 white")

        # ---- step 3: ring3 syscalls with a USER pointer -------------------
        type_text(s, "run /bin/gfx24.ce\n")
        time.sleep(3.0)
        q = shoot(s, "ce")
        print("  screendump: %s" % q)
        r.eq_px(q, 530, 150, (0, 255, 255), "gfx_rect from ring3 (cyan)")
        r.eq_px(q, 630, 332, (120, 128, 128), "gfx_blit RGB24 row 32 col 30")

        # ---- step 4: CE run must not disturb the demo ---------------------
        r.eq_px(q, 150, 170, (255, 0, 0), "demo red block still intact")
        r.eq_px(q, 750, 520, (255, 255, 255), "demo white bar still intact")
    finally:
        kill_qemu(proc)
        if not keep and os.path.isfile(PPM):
            os.remove(PPM)

    print("  %d step(s) failed" % len(r.fails))
    return 1 if r.fails else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
