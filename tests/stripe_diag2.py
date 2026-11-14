#!/usr/bin/env python3
"""stripe_diag2.py -- three-point font RAM snapshot.

boot -> vdiag (dump A: font right after boot reload)
     -> gfx (welcome screen draws)
     -> text Y (roundtrip)
     -> vdiag (dump B: font after roundtrip)
cells dumped at both points too.
"""
import os, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_regress import (boot, cmd, sendkey, type_text, kill_qemu,
                          ppm_to_png, SHOTS, find_qemu)

DUMP_ADDR = 0x010b4f80

def pmemsave(s, addr, size, name):
    cmd(s, "pmemsave %d %d %s" % (addr, size,
        os.path.join(SHOTS, name).replace("\\", "/")), 1.5)

def screendump(s, name):
    ppm = os.path.join(SHOTS, name + ".ppm").replace("\\", "/")
    cmd(s, "screendump %s" % ppm, 1.2)
    ppm_to_png(ppm, os.path.join(SHOTS, name + ".png"))

def main():
    iso = os.path.join(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__))), "kernel_v25", "catos.iso")
    s = boot(iso, find_qemu(None), os.path.abspath(
        os.path.join(SHOTS, "..", "qemu_disk.img")))
    print("monitor connected"); time.sleep(25)

    do = lambda c: (type_text(s, c), sendkey(s, "ret", 1.5))

    # A: boot state
    do("vdiag"); time.sleep(0.8)
    pmemsave(s, DUMP_ADDR, 128, "A_boot_regdump.bin")
    pmemsave(s, 0xB8000, 0x1000, "A_boot_cells.bin")

    # gfx excursion
    do("gfx"); time.sleep(1.0)
    do("text"); sendkey(s, "n", 1.0)     # reject first (matches regress)
    time.sleep(0.8)
    do("text"); sendkey(s, "y", 1.0)     # accept -> roundtrip
    time.sleep(1.5)
    screendump(s, "B_after_text")

    # B: post-roundtrip state
    do("vdiag"); time.sleep(0.8)
    pmemsave(s, DUMP_ADDR, 128, "B_rt_regdump.bin")
    pmemsave(s, 0xB8000, 0x1000, "B_rt_cells.bin")
    kill_qemu()

    for tag in ("A_boot", "B_rt"):
        d = open(os.path.join(SHOTS, tag + "_regdump.bin"), "rb").read()
        print("\n== %s ==" % tag)
        if len(d) >= 105 and d[104] == 0x5A:
            print("Seq01=%02X GC05=%02X GC06=%02X" % (d[0], d[3], d[4]))
            for label, off in (("glyph00", 40), ("glyph20", 56),
                               ("glyph41", 72)):
                print(label + ":", " ".join("%02X" % b for b in d[off:off+16]))
        else:
            print("magic missing", d[:16].hex())
        c = open(os.path.join(SHOTS, tag + "_cells.bin"), "rb").read()
        row0 = "".join(chr(c[2*i]) if 0x20 <= c[2*i] < 0x7F else "."
                       for i in range(40))
        print("cells row0:", row0)
    return 0

if __name__ == "__main__":
    sys.exit(main())
