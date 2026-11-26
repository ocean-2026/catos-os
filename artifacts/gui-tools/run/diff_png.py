#!/usr/bin/env python3
"""Zero-dependency PNG reader + pixel diff between two 800x600 RGB PNGs."""
import struct, zlib, sys


def read_png(path):
    data = open(path, "rb").read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", path
    pos = 8
    w = h = bit = ctype = None
    idat = b""
    while pos < len(data):
        ln = struct.unpack(">I", data[pos:pos + 4])[0]
        typ = data[pos + 4:pos + 8]
        payload = data[pos + 8:pos + 8 + ln]
        if typ == b"IHDR":
            w, h, bit, ctype = struct.unpack(">IIBB", payload[:10])
        elif typ == b"IDAT":
            idat += payload
        elif typ == b"IEND":
            break
        pos += 12 + ln
    assert bit == 8 and ctype == 2, (path, bit, ctype)
    raw = zlib.decompress(idat)
    stride = w * 3
    px = bytearray(w * h * 3)
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        f = raw[p]; p += 1
        line = bytearray(raw[p:p + stride]); p += stride
        if f == 1:
            for i in range(3, stride):
                line[i] = (line[i] + line[i - 3]) & 0xff
        elif f == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xff
        elif f == 3:
            for i in range(stride):
                line[i] = (line[i] + ((line[i - 3] if i >= 3 else 0) + prev[i]) // 2) & 0xff
        elif f == 4:
            for i in range(stride):
                a = line[i - 3] if i >= 3 else 0
                b = prev[i]
                c = prev[i - 3] if i >= 3 else 0
                pp = a + b - c
                pa, pb, pc = abs(pp - a), abs(pp - b), abs(pp - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xff
        px[y * stride:(y + 1) * stride] = line
        prev = line
    return w, h, px


def diff(a, b, tol=0):
    wa, ha, pa = read_png(a)
    wb, hb, pb = read_png(b)
    assert (wa, ha) == (wb, hb), ((wa, ha), (wb, hb))
    n = wa * ha
    diff_px = 0
    color_mismatch = {}
    for i in range(n):
        o = i * 3
        if abs(pa[o] - pb[o]) > tol or abs(pa[o + 1] - pb[o + 1]) > tol \
                or abs(pa[o + 2] - pb[o + 2]) > tol:
            diff_px += 1
            key = (tuple(pa[o:o + 3]), tuple(pb[o:o + 3]))
            color_mismatch[key] = color_mismatch.get(key, 0) + 1
    return wa, ha, n, diff_px, color_mismatch


if __name__ == "__main__":
    a, b = sys.argv[1], sys.argv[2]
    w, h, n, d, mm = diff(a, b)
    print("size=%dx%d pixels=%d diff=%d pct=%.4f%%" %
          (w, h, n, d, 100.0 * d / n))
    top = sorted(mm.items(), key=lambda kv: -kv[1])[:12]
    for (ca, cb), cnt in top:
        print("  qemu=%s preview=%s n=%d" % (ca, cb, cnt))
