#!/usr/bin/env python3
"""Compare kernel BLKGEN serial geometry against .blk.json rects (tol=0)."""
import json, re, sys, collections

JSON = "/home/user/work/gui-tools/artifacts/gui-tools/sample.blk.json"
SERIAL = "/home/user/work/gui-tools/artifacts/gui-tools/run/serial-run.log"

pat = re.compile(r"BLKGEN id=(\d+) x=(-?\d+) y=(-?\d+) w=(-?\d+) h=(-?\d+)")


def load_rects():
    tree = json.load(open(JSON, encoding="utf-8"))
    out = {}
    def walk(n):
        out[n["id"]] = n["rect"]
        for c in n.get("children", []):
            walk(c)
    walk(tree["root"])
    return out


def stages():
    """Split serial dump into per-boot stage blocks of 7 lines."""
    groups, cur = [], []
    for line in open(SERIAL, encoding="latin-1"):
        m = pat.search(line)
        if m:
            cur.append(tuple(int(x) for x in m.groups()))
        elif cur:
            groups.append(cur); cur = []
    if cur:
        groups.append(cur)
    return groups


def main():
    rects = load_rects()
    groups = stages()
    print("stages found:", len(groups))
    for si, grp in enumerate(groups):
        print("\n=== stage %d ===" % si)
        total = matched = 0
        for rec in grp:
            i, x, y, w, h = rec
            j = rects.get(i)
            if j is None:
                continue
            total += 1
            ok = (j["x"], j["y"], j["w"], j["h"]) == (x, y, w, h)
            matched += ok
            print("id=%-2d json=(%3d,%3d,%3d,%3d) kernel=(%3d,%3d,%3d,%3d) %s"
                  % (i, j["x"], j["y"], j["w"], j["h"], x, y, w, h,
                     "OK" if ok else "MISMATCH"))
        print("matched %d/%d" % (matched, total))


if __name__ == "__main__":
    main()
