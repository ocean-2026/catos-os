#!/usr/bin/env python3
"""geom_probe.py -- measure the painted console geometry + palette.

Boots CatOS, waits for the shell prompt in whatever mode it booted, then
dumps one frame and reports:
  * the two dominant colours and their bounding boxes (cell background vs
    frame backdrop -- complaint #4),
  * the painted extent in cells (complaint #3: framebuffer 800x600 vs
    console grid),
  * the backend's own numbers via vdiag / gfx_info / display.

usage: python3 geom_probe.py <iso> <disk> <outdir> [--vga std|vmware]
"""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import qemu_driver as qd


def read_ppm(path):
    with open(path, "rb") as f:
        d = f.read()
    parts = d.split(b"\n", 3)
    w, h = [int(x) for x in parts[1].split()]
    return w, h, parts[3]


def analyse(path):
    w, h, px = read_ppm(path)
    from collections import Counter
    cnt = Counter()
    for y in range(0, h, 3):
        for x in range(0, w, 3):
            i = (y * w + x) * 3
            cnt[(px[i], px[i + 1], px[i + 2])] += 1
    top = cnt.most_common(4)
    print("frame %dx%d dominant: %s" % (w, h, top))
    # bbox of the second colour (usually the cell background)
    for colour, _n in top[:3]:
        minx, maxx, miny, maxy = 10 ** 9, -1, 10 ** 9, -1
        for y in range(0, h, 2):
            for x in range(0, w, 2):
                i = (y * w + x) * 3
                if (px[i], px[i + 1], px[i + 2]) == colour:
                    minx = min(minx, x); maxx = max(maxx, x)
                    miny = min(miny, y); maxy = max(maxy, y)
        cells_w = (maxx - minx + 1) / 8.0
        cells_h = (maxy - miny + 1) / 16.0
        print("  %s bbox x%d..%d y%d..%d  (%.1f x %.1f cells @8x16)"
              % (colour, minx, maxx, miny, maxy, cells_w, cells_h))


def main():
    iso, disk, out = sys.argv[1], sys.argv[2], sys.argv[3]
    vga = "std"
    if "--vga" in sys.argv:
        vga = sys.argv[sys.argv.index("--vga") + 1]
    os.makedirs(out, exist_ok=True)
    extra = ["-vga", vga]
    proc, s = qd.boot(qd.find_qemu(), iso, 4462, disk=disk, extra_args=extra)
    try:
        ok = qd.wait_for_prompt(s, timeout=120.0, poll=3.0)
        print("prompt:", ok)
        time.sleep(1.0)
        p = os.path.join(out, "boot.ppm")
        qd.cmd(s, "screendump %s" % p, wait=1.5)
        if os.path.exists(p):
            analyse(p)
        print("screen text tail:\n" + qd.screen_text(s)[-800:])
        for n, line in enumerate(("vdiag", "gfx_info", "display")):
            qd.type_text(s, line)
            qd.sendkey(s, "ret", 2.5)
            time.sleep(0.8)
            print("\n--- %s ---\n%s" % (line, qd.screen_text(s)[-1400:]))
            qd.cmd(s, "screendump %s" % os.path.join(out, "%02d-%s.ppm" % (n, line)),
                   wait=1.0)
        return 0
    finally:
        qd.kill_qemu(proc)


if __name__ == "__main__":
    sys.exit(main())
