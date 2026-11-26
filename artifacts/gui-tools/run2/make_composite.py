#!/usr/bin/env python3
"""Side-by-side composite: left=preview, right=QEMU. Zero-dependency PNG writer."""
import struct, zlib, sys
from diff_png import read_png


def write_png(path, w, h, rgb):
    raw = bytearray()
    stride = w * 3
    for y in range(h):
        raw.append(0)
        raw += rgb[y * stride:(y + 1) * stride]
    comp = zlib.compress(bytes(raw), 9)

    def chunk(t, payload):
        c = t + payload
        return struct.pack(">I", len(payload)) + c + struct.pack(">I", zlib.crc32(c) & 0xffffffff)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", comp)
    png += chunk(b"IEND", b"")
    open(path, "wb").write(png)


def main():
    left, right, out = sys.argv[1], sys.argv[2], sys.argv[3]
    w, h, pl = read_png(left)
    _, _, pr = read_png(right)
    W = w * 2 + 2
    rgb = bytearray(W * h * 3)
    for y in range(h):
        for x in range(w):
            rgb[(y * W + x) * 3:(y * W + x) * 3 + 3] = pl[(y * w + x) * 3:(y * w + x) * 3 + 3]
            rgb[(y * W + w + 1 + x) * 3:(y * W + w + 1 + x) * 3 + 3] = pr[(y * w + x) * 3:(y * w + x) * 3 + 3]
        div = (y * W + w) * 3
        rgb[div:div + 3] = b"\x55\x5a\x66"
    write_png(out, W, h, rgb)
    print("wrote", out, W, h)


if __name__ == "__main__":
    main()
