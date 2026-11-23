#!/usr/bin/env python3
"""run_light.py -- reproducible light-axis baseline runner + summarizer.

One lock-pinned QEMU batch per PERF_SPEC.md, then a median table grouped by
metric + `kind`.  Every number in artifacts/perf-baseline/light.md traces
back to a `[perf]` line in /home/user/work/catos-bench-runs/<tag>/.run*.log.

Usage:
  python3 bench/run_light.py --tag baseline-light
  python3 bench/run_light.py --tag baseline-light --repeat 3 \
      --disk /home/user/work/catos-os/kernel_v25/catos_disk.img

The build is NOT done here: rebuild first (same command as the spec):
  cd kernel_v25 && BENCH=1 sh build.sh kernel && BENCH=1 sh build.sh iso

Exit code 0 always: the report is evidence, not a gate.  A failed run is
surfaced from runs.json (ok=false) rather than turned into a zero metric.
"""
import argparse
import glob
import hashlib
import json
import os
import re
import statistics
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from qrun import run_batch  # noqa: E402

RUN_ROOT = "/home/user/work/catos-bench-runs"
DEFAULT_DISK = "/home/user/work/catos-os/kernel_v25/catos_disk.img"

PERF_RE = re.compile(r"^\[perf\] (\S+)(.*)$")
BTIME_RE = re.compile(r"^\[btime\] (\d+)ms (.+)$")

KIND_SHELL = {1: "echo", 2: "ls /bin", 3: "edit -c /etc/passwd"}
KIND_RT = {0: "gettid", 1: "write", 2: "file_read"}


def parse_log(path):
    metrics, btime = [], {}
    for line in open(path, "r", errors="replace"):
        m = PERF_RE.match(line.strip())
        if m:
            name, rest = m.group(1), m.group(2)
            kv = {}
            for k, v in re.findall(r"(\w+)=([0-9a-fA-Fx]+)", rest):
                kv[k] = int(v, 0)
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


def median(vals):
    return statistics.median(vals) if vals else None


def collect(outdir):
    """Return rows: {(metric, kind): {field: [values]}}, boot stages, host boot."""
    rows = {}
    boots = []
    host_boot = []
    for log in sorted(glob.glob(os.path.join(outdir, "*.run*.log"))):
        ms, btime = parse_log(log)
        if btime:
            boots.append(btime)
        for name, kv in ms:
            kind = kv.get("kind", 0)
            rows.setdefault((name, kind), []).append(kv)
    mj = os.path.join(outdir, "runs.json")
    if os.path.exists(mj):
        for r in json.load(open(mj)):
            if r.get("host_boot_ms"):
                host_boot.append(r["host_boot_ms"])
    return rows, boots, host_boot


def median_fields(records):
    fields = {}
    keys = set()
    for r in records:
        keys |= set(r)
    for k in keys:
        vals = [r[k] for r in records if k in r]
        if vals:
            fields[k] = median(vals)
    return fields


def derived(f):
    out = {}
    mhz = f.get("mhz")
    cyc = f.get("cycles")
    if mhz and cyc is not None:
        out["us"] = cyc / mhz
        if f.get("count"):
            out["per_call_cycles"] = cyc / f["count"]
            out["per_call_us"] = cyc / mhz / f["count"]
    return out


def iso_info(iso):
    if not os.path.exists(iso):
        return {"bytes": None, "sha8": None}
    h = hashlib.sha256()
    with open(iso, "rb") as fh:
        for chunk in iter(lambda: fh.read(65536), b""):
            h.update(chunk)
    return {"bytes": os.path.getsize(iso), "sha8": h.hexdigest()[:8]}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tag", default="baseline-light")
    ap.add_argument("--cmd", action="append", default=["bench light"])
    ap.add_argument("--disk", default=DEFAULT_DISK)
    ap.add_argument("--repeat", type=int, default=3)
    ap.add_argument("--outdir", default=None)
    ap.add_argument("--iso", default=os.path.join(REPO, "kernel_v25", "catos.iso"))
    ap.add_argument("--artifacts", default=os.path.join(REPO, "artifacts",
                                                        "perf-baseline"))
    ap.add_argument("--collect-only", action="store_true",
                    help="skip qrun; re-summarize an existing run dir")
    args = ap.parse_args()

    disk = args.disk if os.path.exists(args.disk) else None
    if args.collect_only:
        outdir = os.path.join(RUN_ROOT, args.tag)
    else:
        res = run_batch(tag=args.tag, cmds=args.cmd, iso=args.iso, disk=disk,
                        repeat=args.repeat)
        outdir = res["outdir"]
    rows, boots, host_boot = collect(outdir)

    info = iso_info(args.iso)
    nproc = os.cpu_count()
    loadavg = open("/proc/loadavg").read().strip()
    try:
        qver = subprocess.run(["qemu-system-i386", "--version"],
                               capture_output=True, text=True, timeout=10)
        qver = qver.stdout.splitlines()[0] if qver.returncode == 0 else "n/a"
    except Exception:  # noqa: BLE001
        qver = "n/a"

    # ---- build the median table -----------------------------------------
    table = []
    for (name, kind), recs in sorted(rows.items()):
        f = median_fields(recs)
        d = derived(f)
        label = ""
        if name.startswith("shell.cmd") or name == "shell.idle":
            label = KIND_SHELL.get(kind, "other")
        cyc = [r["cycles"] for r in recs if "cycles" in r]
        spread = None
        if cyc:
            med = statistics.median(cyc)
            if med:
                spread = round((max(cyc) - min(cyc)) / med * 100, 1)
        table.append({
            "metric": name, "kind": kind, "label": label, "n": len(recs),
            "cycles": f.get("cycles"), "mhz": f.get("mhz"),
            "us": None if "us" not in d else round(d["us"], 3),
            "count": f.get("count"),
            "per_call_cycles": None if "per_call_cycles" not in d
            else round(d["per_call_cycles"], 1),
            "per_call_us": None if "per_call_us" not in d
            else round(d["per_call_us"], 3),
            "spread_pct": spread,
            "bad": f.get("bad"), "spawn_rc": f.get("spawn_rc"),
        })

    boot_stage = {}
    if boots:
        keys = set()
        for b in boots:
            keys |= set(b)
        boot_stage = {k: median([b[k] for b in boots if k in b]) for k in keys}

    summary = {
        "tag": args.tag, "outdir": outdir, "iso": args.iso,
        "iso_bytes": info["bytes"], "iso_sha8": info["sha8"],
        "qemu": qver, "nproc": nproc, "loadavg": loadavg,
        "repeat": args.repeat, "disk": disk,
        "host_boot_ms": median(host_boot) if host_boot else None,
        "boot_stage": boot_stage, "table": table,
    }

    os.makedirs(args.artifacts, exist_ok=True)
    with open(os.path.join(args.artifacts, "light.json"), "w") as fh:
        json.dump(summary, fh, indent=1)

    # CSV
    cols = ["metric", "kind", "label", "n", "cycles", "mhz", "us", "count",
            "per_call_cycles", "per_call_us", "spread_pct", "bad", "spawn_rc"]
    with open(os.path.join(args.artifacts, "light.csv"), "w") as fh:
        fh.write(",".join(cols) + "\n")
        for r in table:
            fh.write(",".join("" if r[c] is None else str(r[c]) for c in cols)
                     + "\n")

    print(json.dumps({"outdir": outdir, "rows": len(table),
                      "iso": info}, indent=1))


if __name__ == "__main__":
    main()
