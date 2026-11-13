#!/usr/bin/env python3
"""svga24_regress.py -- VMware SVGA II truecolour acceptance (v2.20).

QEMU 11's `-vga vmware` emulates the same 15AD:0405 SVGA II device as
VMware Workstation, so this suite is the on-metal proof for the VMware
graphics path without needing the real hypervisor:

  step 1  `svgainfo` / `display` see the PCI device in text mode
  step 2  `gfx24` paints the fixed 24-bit-colour demo through the SVGA
          FIFO (32bpp XRGB carrying RGB24) -- pixel-exact assertions
  step 3  ring3 `run /bin/gfx24.ce` exercises gfx_info/rect/blit on SVGA
  step 4  `text`+Y returns to a clean 80x25 console (SVGA disabled)
  step 5  `vbe24` (the alias the user actually typed) paints it again
  step 6  `vbe` renders truecolour text with real RGB cell colours

Kill by PID only -- never taskkill /IM (shared machine).
usage: python tests/svga24_regress.py [catos.iso] [--qemu PATH] [--keep]
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
MONITOR_PORT = 4467                 # own port: 4444/4460..4466/4476 taken
SHOT_DIR = os.path.join(HERE, "qemu_shots")
PPM = os.path.join(SHOT_DIR, "svga24.ppm")
DISK = os.path.join(HERE, "qemu_disk_svga24.img")
ISO = os.path.join(REPO, "kernel_v25", "catos.iso")


def win_path(p):
    return os.path.abspath(p).replace("\\", "/")


def drain(s):
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

    def count(self, colour, y0=0, y1=None, step=1):
        """Count pixels of one RGB value in a horizontal band."""
        y1 = self.h if y1 is None else y1
        n = 0
        for y in range(y0, y1, step):
            base = y * self.w * 3
            for x in range(0, self.w, step):
                i = base + x * 3
                if (self.raw[i], self.raw[i + 1], self.raw[i + 2]) == colour:
                    n += 1
        return n

    def __str__(self):
        return "%dx%d max=%d" % (self.w, self.h, self.mx)


def shoot(s, tag):
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


def demo_assertions(r, p, label):
    r.check(label + ": 800x600", (p.w, p.h) == (800, 600),
            "got %dx%d" % (p.w, p.h))
    if (p.w, p.h) != (800, 600):
        return
    r.eq_px(p, 150, 170, (255, 0, 0), label + ": red block")
    r.eq_px(p, 300, 170, (0, 255, 0), label + ": green block")
    r.eq_px(p, 150, 320, (0, 0, 255), label + ": blue block")
    r.eq_px(p, 300, 320, (255, 255, 255), label + ": white block")
    r.eq_px(p, 450, 200, (16, 32, 64), label + ": 24-bit backdrop")
    r.eq_px(p, 400, 440, (127, 0, 128), label + ": gradient midpoint")
    r.eq_px(p, 50, 520, (0, 0, 0), label + ": bar 0 black")
    r.eq_px(p, 750, 520, (255, 255, 255), label + ": bar 7 white")


def back_to_text(s):
    type_text(s, "text")
    sendkey(s, "ret", 1.0)
    sendkey(s, "y", 1.2)
    return screen_text(s)


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
        print("svga24: need QEMU and a built ISO (got %r / %r)" % (qemu, iso))
        return 2

    os.makedirs(SHOT_DIR, exist_ok=True)
    if os.path.isfile(PPM):
        os.remove(PPM)
    if not os.path.exists(DISK):
        with open(DISK, "wb") as fh:
            fh.truncate(64 * 1024 * 1024)     # sparse 64 MiB

    r = Report()
    print("svga24 regression: %s" % iso)
    print("  QEMU: %s  monitor port %d  -vga vmware" % (qemu, MONITOR_PORT))
    proc, s = boot(qemu, iso, MONITOR_PORT, disk=win_path(DISK),
                   extra_args=["-vga", "vmware"])
    try:
        if not wait_for_prompt(s):
            print("  FAIL shell prompt never appeared")
            return 1
        print("  PASS shell prompt")

        # ---- 1: device discovery in text mode ---------------------------
        type_text(s, "svgainfo\n")
        time.sleep(1.0)
        txt = screen_text(s)
        r.check("svgainfo sees SVGA II", "SVGA II adapter : present" in txt)
        r.check("SVGA id 0x90000002", "0x90000002" in txt,
                "" if "0x90000002" in txt else txt[-160:].replace("\n", " "))

        type_text(s, "display\n")
        time.sleep(1.0)
        txt = screen_text(s)
        r.check("display reports VMware graphics HW",
                "VMware SVGA II" in txt)

        # ---- 2: install built-in apps for the ring3 syscall checks ------
        type_text(s, "format")
        sendkey(s, "ret", 1.0)
        sendkey(s, "y", 0.4)
        sendkey(s, "ret", 15.0)
        r.check("format + builtin install", wait_for_prompt(s, timeout=90.0))
        type_text(s, "clear\n")
        time.sleep(0.6)
        type_text(s, "ls /bin\n")
        time.sleep(2.5)
        r.check("/bin/gfx24.ce installed", "gfx24.ce" in screen_text(s).lower())

        # ---- 3: gfx24 on SVGA II ---------------------------------------
        type_text(s, "gfx24\n")
        time.sleep(3.0)
        p = shoot(s, "gfx24")
        print("  screendump: %s" % p)
        demo_assertions(r, p, "gfx24@svga")

        # ---- 4: ring3 drawing syscalls on the SVGA framebuffer ----------
        type_text(s, "run /bin/gfx24.ce\n")
        time.sleep(3.0)
        q = shoot(s, "ce")
        r.eq_px(q, 530, 150, (0, 255, 255), "gfx_rect from ring3 (cyan)")
        r.eq_px(q, 630, 332, (120, 128, 128), "gfx_blit RGB24 row 32 col 30")
        r.eq_px(q, 150, 170, (255, 0, 0), "demo survives ring3 draw")

        # ---- 5: leave graphics cleanly, FIFO actually flushed -----------
        txt = back_to_text(s)
        r.check("text returns to 80x25", "back to 80x25 text mode" in txt)
        type_text(s, "svgainfo\n")
        time.sleep(1.0)
        txt = screen_text(s)
        m = re.search(r"updates=(\d+)", txt)
        n = int(m.group(1)) if m else 0
        r.check("SVGA FIFO issued UPDATE commands", n > 0, "updates=%d" % n)
        fb = re.search(r"framebuffer\s*:\s*0x([0-9A-Fa-f]+)", txt)
        fbhex = fb.group(1) if fb else "0"
        r.check("SVGA framebuffer BAR populated",
                fb is not None and int(fbhex, 16) != 0,
                "fb=0x%s" % fbhex)

        # ---- 6: `vbe24` alias paints the demo again ----------------------
        type_text(s, "vbe24\n")
        time.sleep(3.0)
        p2 = shoot(s, "vbe24")
        demo_assertions(r, p2, "vbe24@svga")

        # ---- 7: `vbe` truecolour text terminal ---------------------------
        back_to_text(s)
        type_text(s, "vbe\n")
        time.sleep(3.0)
        p3 = shoot(s, "vbe")
        r.check("vbe: 800x600", (p3.w, p3.h) == (800, 600),
                "got %dx%d" % (p3.w, p3.h))
        if (p3.w, p3.h) == (800, 600):
            gold = p3.count((255, 215, 0), 0, 160, 2)
            bg = p3.count((16, 48, 80), 0, 160, 2)
            r.check("vbe: real RGB cell colours", gold > 20 and bg > 50,
                    "gold=%d bg=%d" % (gold, bg))
    finally:
        kill_qemu(proc)
        if not keep and os.path.isfile(PPM):
            os.remove(PPM)

    print("  %d step(s) failed" % len(r.fails))
    return 1 if r.fails else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
