#!/usr/bin/env python3
"""report.py -- turn N bench run logs into one median table.

Reads `[perf] <metric> k=v ...` serial lines (see bench/PERF_SPEC.md),
reassembles <key>_hi/<key>_lo pairs, takes the median of every numeric
field across the repeated runs, and derives the human units from the
guest's own calibrated `mhz`:

    us     = cycles / mhz                 (mhz = TSC Hz / 1e6)
    MB/s   = bytes * mhz / cycles         (decimal MB)
    fps    = mhz * 1e6 / cycles_per_frame

usage:
  python3 bench/report.py --tag baseline --outdir /home/user/work/catos-bench-runs/baseline
  python3 bench/report.py --tag baseline --tag optimized --md artifacts/perf/report.md

Exit code 0 always: the report is evidence, not a gate.
"""
import argparse
import glob
import json
import os
import re
import statistics
import sys

PERF_RE = re.compile(r"^\[perf\] (\S+)(.*)$")
BTIME_RE = re.compile(r"^\[btime\] (\d+)ms (.+)$")


def parse_log(path):
    """Return (metrics, btime) from one serial log."""
    metrics = []
    btime = {}
    for line in open(path, "r", errors="replace"):
        m = PERF_RE.match(line.strip())
        if m:
            name, rest = m.group(1), m.group(2)
            kv = {}
            for k, v in re.findall(r"(\w+)=([0-9a-fA-Fx]+)", rest):
                kv[k] = int(v, 0)
            # <key>_hi / <key>_lo -> <key>
            for k in list(kv):
                if k.endswith("_lo"):
                    base = k[:-3]
                    hi = kv.pop(base + "_hi", 0)
                    lo = kv.pop(k)
                    kv[base] = (hi << 32) | lo
            metrics.append((name, kv))
            continue
        b = BTIME_RE.match(line.strip())
        if b:
            btime[b.group(2)] = int(b.group(1))
    return metrics, btime


def group(metrics):
    out = {}
    for name, kv in metrics:
        out.setdefault(name, []).append(kv)
    return out


def med(values):
    return statistics.median(values) if values else None


def summarize(outdirs):
    """{metric: {"fields": {k: median}, "n": run count}} plus boot rows."""
    all_metrics = []
    boots = []
    host_boot = []
    for outdir in outdirs:
        for log in sorted(glob.glob(os.path.join(outdir, "*.run*.log"))):
            ms, btime = parse_log(log)
            all_metrics.extend(ms)
            if "S9 shell prompt ready" in btime:
                boots.append(btime)
        mj = os.path.join(outdir, "runs.json")
        if os.path.exists(mj):
            host_boot += [r.get("host_boot_ms") for r in json.load(open(mj))
                          if r.get("host_boot_ms")]
    grouped = group(all_metrics)
    summ = {}
    for name, rows in grouped.items():
        fields = {}
        keys = set()
        for r in rows:
            keys |= set(r)
        for k in keys:
            vals = [r[k] for r in rows if k in r]
            if vals:
                fields[k] = med(vals)
        summ[name] = {"fields": fields, "n": len(rows)}
    # in-guest boot stages (uptime_ms at each [btime] mark, ms since K0)
    if boots:
        keys = set()
        for b in boots:
            keys |= set(b)
        summ["boot.stage"] = {
            "fields": {k: med([b[k] for b in boots if k in b])
                       for k in keys},
            "n": len(boots)}
    # host-observed boot time (includes GRUB + firmware, unlike [btime])
    if host_boot:
        summ["boot.host"] = {"fields": {"ms": med(host_boot)},
                             "n": len(host_boot)}
    return summ


def derived(name, f):
    """Human units for one metric row, from its own mhz."""
    mhz = f.get("mhz")
    cyc = f.get("cycles")
    out = {}
    if mhz and cyc is not None:
        out["us"] = cyc / mhz
        if f.get("bytes"):
            out["MB/s"] = f["bytes"] * mhz / cyc
        if f.get("count"):
            out["per_call_us"] = cyc / mhz / f["count"]
            out["per_call_cycles"] = cyc / f["count"]
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tag", action="append", default=[],
                    help="tag:outdir (repeatable); default runs/<tag>")
    ap.add_argument("--runs-root", default="/home/user/work/catos-bench-runs")
    ap.add_argument("--md", default=None, help="write markdown here")
    ap.add_argument("--csv", default=None, help="write csv here")
    a = ap.parse_args()
    tags = []
    for t in a.tag:
        tag, _, outdir = t.partition(":")
        tags.append((tag, outdir or os.path.join(a.runs_root, tag)))
    if not tags:
        tags = [("baseline", os.path.join(a.runs_root, "baseline"))]
    rows = []
    for tag, outdir in tags:
        if not os.path.isdir(outdir):
            print("missing run dir %s" % outdir, file=sys.stderr)
            continue
        summ = summarize([outdir])
        for name in sorted(summ):
            f = summ[name]["fields"]
            d = derived(name, f)
            rows.append({
                "tag": tag, "metric": name, "n": summ[name]["n"],
                "cycles": f.get("cycles"), "mhz": f.get("mhz"),
                "us": None if "us" not in d else round(d["us"], 3),
                "bytes": f.get("bytes"), "count": f.get("count"),
                "MB_per_s": None if "MB/s" not in d else round(d["MB/s"], 2),
                "per_call_us": None if "per_call_us" not in d
                else round(d["per_call_us"], 3),
                "ms": f.get("ms"),
            })
    cols = ["tag", "metric", "n", "cycles", "mhz", "us", "ms", "bytes",
            "MB_per_s", "count", "per_call_us"]
    if a.csv:
        os.makedirs(os.path.dirname(a.csv) or ".", exist_ok=True)
        with open(a.csv, "w") as fh:
            fh.write(",".join(cols) + "\n")
            for r in rows:
                fh.write(",".join("" if r[c] is None else str(r[c])
                                  for c in cols) + "\n")
    md = "| " + " | ".join(cols) + " |\n|" + "|".join(
        ["---"] * len(cols)) + "|\n"
    for r in rows:
        md += "| " + " | ".join("" if r[c] is None else str(r[c])
                                for c in cols) + " |\n"
    print(md)
    if a.md:
        os.makedirs(os.path.dirname(a.md) or ".", exist_ok=True)
        with open(a.md, "w") as fh:
            fh.write(md)


if __name__ == "__main__":
    main()
