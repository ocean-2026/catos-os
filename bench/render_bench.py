#!/usr/bin/env python3
"""render_bench.py -- gfx24 render-path baseline/optimized comparison driver.

Subcommands:
  bench <out.json> [--repeat N]   run `bench gfx` through qrun N times,
                                  median gfx.frame / gfx.char cycles -> JSON.
  capture <ppm>                   boot the current ISO, paint the fixed
                                  kernel `gfx24` demo, screendump P6 PPM.
  diff <a.ppm> <b.ppm>           byte-compare two dumps; report mismatched
                                  pixels in the demo region (y >= 120) and
                                  full-frame counts, plus region md5s.

The render demo lives at y >= 120 (see gfx24_demo in be_vbe24.c); the shell
prompt / echo rows above it are excluded from the pixel comparison because
cursor blink and prompt text are not part of the rendering path under test.

Reproduce:
  cd kernel_v25 && BENCH=1 sh build.sh kernel && BENCH=1 sh build.sh iso
  python3 bench/render_bench.py  bench  artifacts/perf-render/base.json
  python3 bench/render_bench.py  capture artifacts/perf-render/base.ppm
  # ... optimize kernel_v25/src/be_vbe24.c, rebuild ...
  python3 bench/render_bench.py  bench  artifacts/perf-render/opt.json
  python3 bench/render_bench.py  capture artifacts/perf-render/opt.ppm
  python3 bench/render_bench.py  diff   artifacts/perf-render/base.ppm \
                                        artifacts/perf-render/opt.ppm
"""
import argparse
import glob
import hashlib
import json
import os
import re
import statistics
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(REPO, "tests"))

import qrun  # noqa: E402
import qemu_driver  # noqa: E402

ISO = os.path.join(REPO, "kernel_v25", "catos.iso")
RUN_ROOT = "/home/user/work/catos-bench-runs"
DEMO_Y0 = 120


# ---- gfx bench through qrun ---------------------------------------------

def _parse_run(logpath):
    out = {}
    if not os.path.isfile(logpath):
        return out
    for line in open(logpath, errors="replace"):
        m = re.match(r"\[perf\] (\S+) (.*)", line.strip())
        if not m:
            continue
        metric, kv = m.group(1), m.group(2)
        d = dict(re.findall(r"(\w+)=([0-9a-fA-Fx]+)", kv))
        if "cycles_hi" in d and "cycles_lo" in d:
            d["cycles"] = (int(d["cycles_hi"]) << 32) | int(d["cycles_lo"])
        for k in ("frames", "count", "cols", "rows", "mhz"):
            if k in d:
                d[k] = int(d[k])
        for k in ("cycles_glyph", "cycles_flush", "cycles_fetch"):
            if k + "_hi" in d:
                d[k] = ((int(d[k + "_hi"]) << 32) | int(d[k + "_lo"]))
        out[metric] = d
    return out


def run_gfx_bench(tag, repeat=3):
    res = qrun.run_batch(tag=tag, cmds=["bench gfx"], iso=ISO, disk=None,
                         repeat=repeat, port_base=4630, timeout=90,
                         end_count=1)
    series = {"gfx.frame": [], "gfx.char": []}
    per_run = []
    for r in res["runs"]:
        parsed = _parse_run(r["log"])
        per_run.append(parsed)
        for k in series:
            if k in parsed and "cycles" in parsed[k]:
                series[k].append(parsed[k])
    med = {}
    for k, rows in series.items():
        if not rows:
            med[k] = None
            continue
        cyc = [x["cycles"] for x in rows]
        m = dict(rows[0])
        m["cycles_median"] = statistics.median(cyc)
        m["cycles_all"] = cyc
        m["spread"] = ((max(cyc) - min(cyc)) / m["cycles_median"]
                        if m["cycles_median"] else 0.0)
        med[k] = m
    return {"tag": tag, "runs": per_run, "median": med}


# ---- screendump capture ---------------------------------------------------

def capture_ppm(ppm_path, port=4631):
    qemu = qrun._qemu_bin()
    proc, sock = qemu_driver.boot(qemu, ISO, port, disk=None,
                                 extra_args=["-vga", "std"])
    try:
        time.sleep(14)                      # settle to shell-ready
        qemu_driver.type_text(sock, "gfx24")
        qemu_driver.sendkey(sock, "ret", 1.0)
        time.sleep(3.0)
        os.makedirs(os.path.dirname(ppm_path) or ".", exist_ok=True)
        if os.path.isfile(ppm_path):
            os.remove(ppm_path)
        # QEMU capture lags a frame: shoot twice, keep the last.
        for _ in range(2):
            qemu_driver.cmd(sock, "screendump " + os.path.abspath(ppm_path),
                            0.8)
            time.sleep(1.6)
        if not os.path.isfile(ppm_path):
            raise RuntimeError("screendump produced no PPM: %s" % ppm_path)
    finally:
        qemu_driver.kill_qemu(proc)
    return ppm_path


# ---- PPM diff --------------------------------------------------------------

class Ppm(object):
    def __init__(self, path):
        data = open(path, "rb").read()
        m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+(\d+)\s", data)
        if not m:
            raise RuntimeError("not a P6 PPM: %r" % data[:32])
        self.w, self.h, self.mx = (int(v) for v in m.groups())
        self.raw = data[m.end():m.end() + self.w * self.h * 3]

    def region(self, y0):
        return self.raw[y0 * self.w * 3:]

    def md5_region(self, y0):
        return hashlib.md5(self.region(y0)).hexdigest()


def diff_ppm(a_path, b_path, y0=DEMO_Y0):
    a, b = Ppm(a_path), Ppm(b_path)
    if (a.w, a.h) != (b.w, b.h):
        return {"size_mismatch": [a.w, a.h, b.w, b.h]}
    ra, rb = a.region(y0), b.region(y0)
    mism = sum(1 for x, y in zip(ra, rb) if x != y)
    px_mism = mism // 3
    full_mism = sum(1 for x, y in zip(a.raw, b.raw) if x != y) // 3
    return {
        "size": [a.w, a.h],
        "demo_y0": y0,
        "demo_region_bytes": len(ra),
        "demo_pixel_mismatch": px_mism,
        "demo_byte_mismatch": mism,
        "full_pixel_mismatch": full_mism,
        "md5_demo_a": a.md5_region(y0),
        "md5_demo_b": b.md5_region(y0),
        "identical_demo": px_mism == 0,
    }


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)

    p_b = sub.add_parser("bench")
    p_b.add_argument("out_json")
    p_b.add_argument("--repeat", type=int, default=3)
    p_b.add_argument("--tag", default="render-bench")

    p_c = sub.add_parser("capture")
    p_c.add_argument("ppm")

    p_d = sub.add_parser("diff")
    p_d.add_argument("a")
    p_d.add_argument("b")

    a = ap.parse_args()

    if a.cmd == "bench":
        res = run_gfx_bench(a.tag, a.repeat)
        os.makedirs(os.path.dirname(a.out_json) or ".", exist_ok=True)
        with open(a.out_json, "w") as fh:
            json.dump(res, fh, indent=1)
        print(json.dumps({k: (None if v is None else
                              {"cycles": v["cycles_median"],
                               "spread": round(v["spread"], 4),
                               "all": v["cycles_all"]})
                          for k, v in res["median"].items()}, indent=1))
    elif a.cmd == "capture":
        capture_ppm(a.ppm)
        print("captured", a.ppm)
    elif a.cmd == "diff":
        print(json.dumps(diff_ppm(a.a, a.b), indent=1))


if __name__ == "__main__":
    main()
