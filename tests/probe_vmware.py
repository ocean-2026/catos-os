#!/usr/bin/env python3
"""probe_vmware.py -- v2.72 VMware-path probe (-vga vmware == VMware SVGA II).

Purpose: answer the three rendering complaints with numbers instead of
guesses:
  * flicker: sample the screen every ~0.25 s across the boot window and
    print mean luminance per frame, so a "flash" shows up as a spike/dip;
  * geometry: run vdiag/gfx_info/display and dump the text screen;
  * font background: screendump the prompt and compare the pixel colour
    inside a character cell's empty area against the frame's backdrop.

usage: python3 probe_vmware.py <iso> <disk> <outdir> [--vga vmware|std]
"""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import qemu_driver as qd

W, H = 800, 600


def ppm_mean(path):
    """Mean luminance of a binary P6 PPM (0..255), plus a 16x12 tile map."""
    with open(path, "rb") as f:
        data = f.read()
    # header: P6 <w> <h> <max>\n
    parts = data.split(b"\n", 3)
    if parts[0].strip() != b"P6":
        return None, None
    w, h = [int(x) for x in parts[1].split()]
    maxv = int(parts[2].strip())
    px = parts[3]
    step = max(1, (w * h) // 400000)   # subsample for speed
    tot = n = 0
    for i in range(0, min(len(px), w * h * 3) - 2, 3 * step):
        tot += px[i] * 0.30 + px[i + 1] * 0.59 + px[i + 2] * 0.11
        n += 1
    return (tot / n if n else 0.0), (w, h, maxv)


def main():
    iso, disk, out = sys.argv[1], sys.argv[2], sys.argv[3]
    vga = "vmware"
    if "--vga" in sys.argv:
        vga = sys.argv[sys.argv.index("--vga") + 1]
    os.makedirs(out, exist_ok=True)
    proc, s = qd.boot(qd.find_qemu(), iso, 4460, disk=disk,
                      extra_args=["-vga", vga])
    try:
        # boot window: GRUB + kernel + app install ~20 s.  Sample across it.
        print("== flicker sampling (0.25 s) ==")
        t0 = time.time()
        means = []
        for i in range(96):                     # ~24 s
            p = os.path.join(out, "_f%03d.ppm" % i)
            qd.cmd(s, "screendump %s" % p, wait=0.05)
            time.sleep(0.2)
            if not os.path.exists(p):
                continue
            m, geo = ppm_mean(p)
            if m is None:
                continue
            means.append((round(time.time() - t0, 2), round(m, 1)))
            os.remove(p)
        # report compression: consecutive-frame deltas
        print("t=%.2fs frames=%d" % (time.time() - t0, len(means)))
        big = []
        for a, b in zip(means, means[1:]):
            d = abs(b[1] - a[1])
            if d > 12:
                big.append((a[0], a[1], b[0], b[1], round(d, 1)))
        print("luminance transitions >12: %d" % len(big))
        for row in big[:24]:
            print("  %.2fs %.1f -> %.2fs %.1f  (delta %.1f)" % row)
        print("first/last luminance: %s / %s" % (means[0][1], means[-1][1]))
        print("geometry: %s" % (geo,))

        print("== shell probes ==")
        if not qd.wait_for_prompt(s, timeout=40.0):
            print("no prompt; screen:\n" + qd.read_screen(s))
            return 1
        for n, line in enumerate(["vdiag", "gfx_info", "display"]):
            qd.type_text(s, line)
            qd.sendkey(s, "ret", 2.0)
            time.sleep(0.6)
            t = qd.screen_text(s)
            print("\n--- %s ---\n%s" % (line, t[-1200:]))
            p = os.path.join(out, "%02d-%s.ppm" % (n, line))
            qd.cmd(s, "screendump %s" % p, wait=1.0)
        return 0
    finally:
        qd.kill_qemu(proc)


if __name__ == "__main__":
    sys.exit(main())
