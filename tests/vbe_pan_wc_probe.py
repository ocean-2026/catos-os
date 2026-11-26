#!/usr/bin/env python3
"""vbe_pan_wc_probe.py -- P2-D7 32bpp pan / MTRR-WC pixel-level probe.

Kernel side (be_vbe.c vbe_selftest, reached via `vdiag` in CON_MODE_VBE):
  * writes four known dwords to an off-screen row, drains WC (sfence +
    wbinvd) and reads them back, comparing the XRGB mask;
  * when HW panning is active, marks logical row 0, scrolls one line and
    verifies the marker follows the viewport to physical row pan_off;
  * leaves a verdict swatch in visible row 1:
      green 0x00FF00  readback OK and pan checked OK
      yellow 0xFFFF00  readback OK, pan n/a (re-render backend / wrap)
      red   0xFF0000  any pixel mismatch

This probe switches to the vbe backend, runs vdiag, dumps the framebuffer
and samples the swatch pixel.  It verifies only; it changes no paint path.

Usage:
  python tests/vbe_pan_wc_probe.py [catos.iso]
"""
import os
import re
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
sys.path.insert(0, HERE)

from qemu_driver import (kill_qemu, cmd, find_qemu, force_text, screen_text,
                         sendkey, type_text, wait_for_prompt)
from qemu_regress import ppm_to_png

PORT = 4464
DISK = os.path.join(HERE, "qemu_disk_vbepe.img")
ART = os.path.join(ROOT, "artifacts", "v282", "p1-2-d7")

SWATCH_X, SWATCH_Y = 8, 24          # center of the 16x16 swatch, row 1


class Ppm(object):
    def __init__(self, path):
        data = open(path, "rb").read()
        m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+(\d+)\s", data)
        if not m:
            raise RuntimeError("not a P6 PPM: %r" % data[:32])
        self.w, self.h, self.mx = (int(v) for v in m.groups())
        self.raw = data[m.end():m.end() + self.w * self.h * 3]

    def px(self, x, y):
        i = (y * self.w + x) * 3
        return tuple(self.raw[i:i + 3])


def boot_direct(qemu, iso, port, disk):
    log = os.path.join(ART, "vbe_pe_qemu.log")
    # Stage the disk onto a local fs: the workspace fs does not honour QEMU's
    # OFD write lock (same workaround as qemu_driver._stage_image).
    stage_dir = os.path.join(tempfile.gettempdir(), "catos_d7_stage")
    os.makedirs(stage_dir, exist_ok=True)
    stage_disk = os.path.join(stage_dir, os.path.basename(disk))
    if disk:
        import shutil
        shutil.copy2(disk, stage_disk)
    cmdline = [qemu, "-m", "256", "-no-reboot", "-net", "none",
               "-cdrom", iso, "-boot", "order=d", "-display", "none",
               "-vga", "std",
               "-monitor", "tcp:127.0.0.1:%d,server,nowait" % port]
    if disk:
        cmdline += ["-drive", "file=%s,format=raw,if=ide" % stage_disk]
    err = open(log, "wb")
    proc = subprocess.Popen(cmdline, stdout=subprocess.DEVNULL, stderr=err)
    for _ in range(40):
        if proc.poll() is not None:
            err.flush()
            raise RuntimeError("qemu exited rc=%r (see %s)" % (proc.returncode, log))
        try:
            return proc, socket.create_connection(("127.0.0.1", port), timeout=5)
        except OSError:
            time.sleep(2)
    kill_qemu(proc)
    raise RuntimeError("no monitor on port %d" % port)


def main():
    args = sys.argv[1:]
    iso = None
    while args:
        a = args.pop(0)
        if not a.startswith("--"):
            iso = a
    iso = os.path.abspath(iso or os.path.join(ROOT, "kernel_v25", "catos.iso"))
    qemu = find_qemu() or "/home/user/bin/qemu-system-i386"
    if not qemu or not os.path.isfile(iso):
        print("vbe-pe: need QEMU and a built ISO (got %r)" % iso)
        return 2

    os.makedirs(ART, exist_ok=True)
    if not os.path.exists(DISK):
        with open(DISK, "wb") as f:
            f.truncate(64 * 1024 * 1024)

    proc, s = boot_direct(qemu, iso, PORT, disk=DISK)
    results = []
    try:
        force_text(s)
        if not wait_for_prompt(s):
            print("FATAL: shell prompt never appeared")
            return 3

        # Switch to the 32bpp vbe backend, then run the diagnostic selftest.
        type_text(s, "gfx vbe")
        sendkey(s, "ret", 3.0)
        time.sleep(2.0)
        type_text(s, "vdiag")
        sendkey(s, "ret", 1.0)
        time.sleep(2.0)

        stage = os.path.join(tempfile.gettempdir(), "catos_d7_shots")
        os.makedirs(stage, exist_ok=True)
        ppm = os.path.join(stage, "vbe-pe.ppm")
        png = os.path.join(ART, "vbe-pan-wc.png")
        if os.path.exists(ppm):
            os.remove(ppm)
        for _ in range(2):                    # capture lag: dump twice
            cmd(s, "screendump %s" % ppm, 0.8)
            time.sleep(1.6)
        if not (os.path.exists(ppm) and os.path.getsize(ppm) > 0):
            print("FAIL dump: no PPM")
            return 1
        img = Ppm(ppm)
        col = img.px(SWATCH_X, SWATCH_Y)
        print("swatch pixel at (%d,%d): %s (%dx%d)"
              % (SWATCH_X, SWATCH_Y, col, img.w, img.h))

        r, g, b = col
        if r > 150 and g < 100 and b < 100:
            results.append(("vbe-pe", False, "red swatch: pixel mismatch"))
        elif g > 150 and r < 100:
            results.append(("vbe-pe", True, "green swatch: readback + pan OK"))
        elif r > 150 and g > 150 and b < 100:
            results.append(("vbe-pe", True, "yellow swatch: readback OK, pan n/a"))
        else:
            results.append(("vbe-pe", False, "unexpected swatch color %s" % (col,)))

        ppm_to_png(ppm, png)
        print("screenshot: %s" % png)
    finally:
        kill_qemu(proc)

    fails = [n for n, ok, _ in results if not ok]
    for n, ok, note in results:
        print("%-4s %-10s %s" % ("PASS" if ok else "FAIL", n, note))
    print("\nsteps=%d ok=%d fail=%d" % (len(results),
                                        len(results) - len(fails), len(fails)))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
