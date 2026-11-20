#!/usr/bin/env python3
"""Locate the white mouse arrow (pure-white pixel clusters) in a PPM."""
import sys

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
    return vals[0], vals[1], d[i:i + vals[0]*vals[1]*3]

ppm = sys.argv[1]
w, h, px = read_ppm(ppm)
minx = miny = 10**9; maxx = maxy = -1; cnt = 0
for y in range(h):
    row = px[y*w*3:(y+1)*w*3]
    for x in range(w):
        r, g, b = row[x*3], row[x*3+1], row[x*3+2]
        if r > 240 and g > 240 and b > 240:
            cnt += 1
            if x < minx: minx = x
            if x > maxx: maxx = x
            if y < miny: miny = y
            if y > maxy: maxy = y
print("white pixels:", cnt, "bbox:", (minx, miny, maxx, maxy) if cnt else None)
