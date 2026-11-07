#!/usr/bin/env python3
"""stripe_diag.py -- capture data during the gfx->text stripe frame.

Flow: boot -> gfx -> text(y) -> screendump(striped) -> pmemsave B8000
      -> vdiag (fills vga_reg_dump with font/regs readback) -> pmemsave dump
      -> screendump(after vdiag, should be clean)
"""
import os, re, socket, struct, subprocess, sys, time, zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_regress import (boot, cmd, sendkey, type_text, kill_qemu,
                          ppm_to_png, SHOTS, find_qemu)

DUMP_ADDR = 0x010b4f80      # vga_reg_dump (from previous session's nm)

def pmemsave(s, addr, size, name):
    path = os.path.join(SHOTS, name).replace("\\", "/")
    cmd(s, "pmemsave %d %d %s" % (addr, size, path), 1.5)
    return path

def screendump(s, name):
    ppm = os.path.join(SHOTS, name + ".ppm").replace("\\", "/")
    cmd(s, "screendump %s" % ppm, 1.2)
    png = os.path.join(SHOTS, name + ".png")
    ppm_to_png(ppm, png)
    return png

def main():
    iso = os.path.join(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__))), "kernel_v25", "catos.iso")
    s = boot(iso, find_qemu(None), os.path.abspath(
        os.path.join(SHOTS, "..", "qemu_disk.img")))
    print("monitor connected")
    time.sleep(25)

    do = lambda c: (type_text(s, c), sendkey(s, "ret", 1.5))

    print("== gfx =="); do("gfx"); time.sleep(1)
    print("== text y =="); do("text"); sendkey(s, "y", 1.0)
    time.sleep(1.5)                      # > 4 blink periods
    screendump(s, "D0_striped_1")
    screendump(s, "D1_striped_2")        # 2nd dump ~3s after: still striped?
    pmemsave(s, 0xB8000, 0x1000, "D2_cells.bin")

    print("== vdiag =="); do("vdiag"); time.sleep(1.0)
    screendump(s, "D3_vdiag")
    pmemsave(s, DUMP_ADDR, 128, "D4_regdump.bin")
    kill_qemu()

    # ---- analysis ----
    cells = open(os.path.join(SHOTS, "D2_cells.bin"), "rb").read()
    rows = []
    for y in (20, 22, 24):               # rows far below any text
        row = [(cells[2 * (y * 80 + x)], cells[2 * (y * 80 + x) + 1])
               for x in range(8)]
        rows.append("row%d: " % y + " ".join("%02X/%02X" % c for c in row))
    print("\n".join(rows))

    d = open(os.path.join(SHOTS, "D4_regdump.bin"), "rb").read()
    if len(d) >= 105 and d[104] == 0x5A:
        print("Seq01=%02X Seq03=%02X Seq04=%02X GC05=%02X GC06=%02X "
              "AR10=%02X AR12=%02X" % (d[0], d[1], d[2], d[3], d[4],
                                       d[31], d[33]))
        print("CRTC09=%02X CRTC13=%02X" % (d[5 + 9], d[5 + 0x13]))
        for label, off in (("glyph00", 40), ("glyph20", 56),
                           ("glyph41", 72), ("glyphFF", 88)):
            print(label + ":", " ".join("%02X" % b for b in d[off:off+16]))
    else:
        print("regdump magic missing! len=%d head=%s" % (len(d), d[:16].hex()))
    return 0

if __name__ == "__main__":
    sys.exit(main())
