#!/usr/bin/env python3
"""Compare kernel BLKGEN serial geometry against .blk.json rects (tol=0)."""
import json, re, sys

JSON = sys.argv[1] if len(sys.argv) > 1 else \
    "/home/user/work/gui-tools/tools/blkhtml/samples/settings.blk.json"
SERIAL = sys.argv[2] if len(sys.argv) > 2 else \
    "/home/user/work/gui-tools/artifacts/gui-tools/run2/serial-run.log"

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
    print("json blocks:", len(rects), " stages:", len(groups))
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
        # root id=0: kernel root set to 800x600 by blkgen_build
        r0 = rects.get(0)
        print("id= 0 json=(%d,%d,%d,%d) kernel=root 800x600 %s"
              % (r0["x"], r0["y"], r0["w"], r0["h"],
                 "OK" if (r0["w"], r0["h"]) == (800, 600) else "MISMATCH"))
        print("matched %d/%d non-root" % (matched, total))


if __name__ == "__main__":
    main()
