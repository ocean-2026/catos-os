#!/usr/bin/env python3
"""Crop + 3x zoom a region of a PPM screendump for inspection (no PIL)."""
import struct, zlib, sys

def read_ppm(p):
    d = open(p, "rb").read()
    assert d[:2] == b"P6"
    vals = []; i = 2
    while len(vals) < 3:
        if d[i:i+1].isspace(): i += 1; continue
        if d[i:i+1] == b"#":
            while d[i:i+1] not in (b"\n", b""): i += 1
            continue
        j = i
        while not d[j:j+1].isspace(): j += 1
        vals.append(int(d[i:j])); i = j
    i += 1
    w, h = vals[0], vals[1]
    return w, h, d[i:i + w*h*3]

def write_png(p, w, h, px):
    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)
    raw = b"".join(b"\x00" + px[y*w*3:(y+1)*w*3] for y in range(h))
    open(p, "wb").write(b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw, 6))
        + chunk(b"IEND", b""))

ppm, out = sys.argv[1], sys.argv[2]
x0, y0, x1, y1 = (int(v) for v in sys.argv[3:7])
w, h, px = read_ppm(ppm)
cw, ch = (x1-x0)*3, (y1-y0)*3
op = bytearray()
for y in range(y0, y1):
    row = bytearray()
    for z in range(3):
        row += px[(y*w + x0)*3:(y*w + x1)*3]
    op += row
write_png(out, cw, ch, bytes(op))
print("wrote", out, cw, "x", ch)
