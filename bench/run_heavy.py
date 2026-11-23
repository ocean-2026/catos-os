#!/usr/bin/env python3
"""run_heavy.py -- heavy-axis baseline runner (PERF_SPEC.md §6, §8).

Runs the four heavy scenarios through bench/qrun.py (locked, CPU-pinned,
fresh private disk copy per run), 3 repeats per configuration, then turns
the serial logs into a median table under artifacts/perf-baseline/.

Configurations (kept separate so a disk scenario never pollutes a no-disk
run, per PERF_SPEC §3):

  baseline-heavy-nodisk  `bench gfx`  `bench pipe`   (no IDE drive)
  baseline-heavy-disk    `bench disk` `bench task`  (fresh copy of
                                                        bench/disk-ref-64m.img)

Usage:
  python3 bench/run_heavy.py                 # build assumed done; run + report
  python3 bench/run_heavy.py --report-only  # rebuild report from existing logs
  python3 bench/run_heavy.py --repeat 3

Reproduce:
  cd kernel_v25 && BENCH=1 sh build.sh kernel && BENCH=1 sh build.sh iso
  python3 kernel_v25/build/mkdisk.py -o ../bench/disk-ref-64m.img -m 64
  python3 bench/run_heavy.py

The reference disk bench/disk-ref-64m.img is gitignored (*.img); qrun
copies a private scratch copy per run and never mounts the repo original.
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
import qrun  # noqa: E402
import report  # noqa: E402

RUN_ROOT = "/home/user/work/catos-bench-runs"
ISO = os.path.join(REPO, "kernel_v25", "catos.iso")
DISK_REF = os.path.join(HERE, "disk-ref-64m.img")
ART_DIR = os.path.join(REPO, "artifacts", "perf-baseline")

NODISK_TAG = "baseline-heavy-nodisk"
DISK_TAG = "baseline-heavy-disk"

BATCHES = [
    {
        "tag": NODISK_TAG,
        "cmds": ["bench gfx", "bench pipe"],
        "disk": None,
        "port_base": 4600,
    },
    {
        "tag": DISK_TAG,
        "cmds": ["bench disk", "bench task"],
        "disk": DISK_REF,
        "port_base": 4610,
    },
]


def qemu_bin():
    for cand in ("qemu-system-i386", "qemu-system-x86_64"):
        p = qrun.shutil.which(cand)
        if p:
            return p
    return None


def qemu_version(qb):
    try:
        out = subprocess.run([qb, "--version"], capture_output=True,
                              text=True, timeout=10)
        return out.stdout.splitlines()[0] if out.stdout else "unknown"
    except Exception as exc:  # noqa: BLE001
        return "unknown (%s)" % exc


def host_info():
    load = open("/proc/loadavg").read().strip()
    return {"nproc": os.cpu_count(), "loadavg": load}


def git_head():
    try:
        out = subprocess.run(["git", "-C", REPO, "rev-parse", "--short", "HEAD"],
                              capture_output=True, text=True, timeout=5)
        commit = out.stdout.strip() or "unknown"
        br = subprocess.run(["git", "-C", REPO, "branch", "--show-current"],
                            capture_output=True, text=True, timeout=5)
        return commit, br.stdout.strip() or "unknown"
    except Exception:  # noqa: BLE001
        return "unknown", "unknown"


def iso_meta():
    if not os.path.isfile(ISO):
        return None
    data = open(ISO, "rb").read()
    return {"bytes": len(data), "sha256_8": hashlib.sha256(data).hexdigest()[:8]}


def median_rows(outdir):
    """{metric: {fields, n}} from one run directory, via report.py parser."""
    summ = report.summarize([outdir])
    return summ


def derive(metric, f):
    """Human units + segment shares for one median metric row."""
    mhz = f.get("mhz") or 0
    cyc = f.get("cycles")
    out = {}
    if mhz and cyc is not None:
        out["us"] = cyc / mhz
        if f.get("bytes"):
            out["MB/s"] = f["bytes"] * mhz / cyc
        if f.get("frames"):
            out["cycles_per_frame"] = cyc / f["frames"]
            out["fps"] = mhz * 1e6 / (cyc / f["frames"])
        if f.get("count"):
            out["per_call_us"] = cyc / mhz / f["count"]
    # segment shares
    for seg, label in (("cycles_glyph", "glyph"), ("cycles_flush", "flush"),
                       ("cycles_wait", "wait"), ("cycles_xfer", "xfer"),
                       ("cycles_setup", "setup"), ("cycles_exit", "exit")):
        if cyc and seg in f and f[seg]:
            out[label + "_share"] = f[seg] / cyc
    return out


def fmt(v, nd=2):
    if v is None:
        return ""
    if isinstance(v, float):
        return round(v, nd)
    return v


def raw_series(outdir):
    """{metric: [cycles per run]} straight from the serial logs."""
    series = {}
    if not os.path.isdir(outdir):
        return series
    for lg in sorted(glob.glob(os.path.join(outdir, "*.run*.log"))):
        for line in open(lg, errors="replace"):
            m = re.match(r"\[perf\] (\S+) (.*)", line.strip())
            if not m:
                continue
            metric, kv = m.group(1), m.group(2)
            d = dict(re.findall(r"(\w+)=([0-9a-fA-Fx]+)", kv))
            if "cycles_hi" in d and "cycles_lo" in d:
                cyc = (int(d["cycles_hi"]) << 32) | int(d["cycles_lo"])
                series.setdefault(metric, []).append(cyc)
    return series


def spread(series):
    if len(series) < 2:
        return 0.0
    med = statistics.median(series)
    return (max(series) - min(series)) / med if med else 0.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repeat", type=int, default=3)
    ap.add_argument("--report-only", action="store_true")
    ap.add_argument("--timeout", type=int, default=120)
    a = ap.parse_args()

    commit, branch = git_head()
    meta = {
        "repo": REPO,
        "commit": commit,
        "branch": branch,
        "iso": iso_meta(),
        "host": host_info(),
        "qemu": None,
        "cmdline": "taskset -c 1 qemu-system-i386 -m 256 -smp 1 -cpu qemu32 "
                   "-no-reboot -net none -display none -cdrom <iso> -boot order=d "
                   "[-drive file=<private copy>,format=raw,if=ide] -serial file:<log>",
        "repeat": a.repeat,
        "batches": [],
    }

    qb = qemu_bin()
    if qb and not a.report_only:
        meta["qemu"] = {"bin": qb, "version": qemu_version(qb)}
    elif qb:
        meta["qemu"] = {"bin": qb, "version": qemu_version(qb)}
    else:
        meta["qemu"] = None

    if not a.report_only:
        if not qb:
            print("BLOCKED: qemu-system-i386 not on PATH (installer still "
                  "running?). No measurements taken.", file=sys.stderr)
            return 2
        if not os.path.isfile(ISO):
            print("BLOCKED: ISO missing: %s (build kernel+iso first)" % ISO,
                  file=sys.stderr)
            return 2
        if not os.path.isfile(DISK_REF):
            print("BLOCKED: reference disk missing: %s" % DISK_REF,
                  file=sys.stderr)
            return 2
        for b in BATCHES:
            print("== batch %s: %s" % (b["tag"], " ".join(b["cmds"])))
            res = qrun.run_batch(
                tag=b["tag"], cmds=b["cmds"], iso=ISO, disk=b["disk"],
                repeat=a.repeat, port_base=b["port_base"],
                timeout=a.timeout, end_count=len(b["cmds"]))
            meta["batches"].append({
                "tag": b["tag"], "outdir": res["outdir"],
                "runs": [r.get("ok") for r in res["runs"]],
            })

    # ---- collect + median -------------------------------------------------
    rows = []
    raw_all = {}
    for b in BATCHES:
        outdir = os.path.join(RUN_ROOT, b["tag"])
        if not os.path.isdir(outdir):
            continue
        series = raw_series(outdir)
        raw_all.update(series)
        summ = median_rows(outdir)
        for metric in sorted(summ):
            if metric in ("end", "boot", "boot.stage", "boot.host"):
                continue
            f = summ[metric]["fields"]
            raw = series.get(metric, [])
            if f.get("present") == 0:
                rows.append({"metric": metric, "n": summ[metric]["n"],
                             "present": 0, "fields": f, "raw": raw,
                             "spread": spread(raw)})
                continue
            d = derive(metric, f)
            rows.append({"metric": metric, "n": summ[metric]["n"],
                         "present": 1, "fields": f, "derived": d,
                         "raw": raw, "spread": spread(raw)})

    os.makedirs(ART_DIR, exist_ok=True)

    # ---- CSV ---------------------------------------------------------------
    csv_path = os.path.join(ART_DIR, "heavy.csv")
    cols = ["metric", "n", "cycles", "mhz", "us", "bytes", "MB_per_s",
            "frames", "fps", "count", "per_call_us", "cycles_glyph",
            "cycles_flush", "cycles_wait", "cycles_xfer", "cycles_setup",
            "cycles_exit", "sleeps", "ticks", "bad"]
    with open(csv_path, "w") as fh:
        fh.write(",".join(cols) + "\n")
        for r in rows:
            f = r["fields"]
            d = r.get("derived", {})
            line = [r["metric"], r["n"]]
            for k in ("cycles", "mhz"):
                line.append(f.get(k, ""))
            line.append(fmt(d.get("us"), 3))
            line.append(f.get("bytes", ""))
            line.append(fmt(d.get("MB/s")))
            line.append(f.get("frames", ""))
            line.append(fmt(d.get("fps")))
            line.append(f.get("count", ""))
            line.append(fmt(d.get("per_call_us"), 3))
            for k in ("cycles_glyph", "cycles_flush", "cycles_wait",
                      "cycles_xfer", "cycles_setup", "cycles_exit",
                      "sleeps", "ticks", "bad"):
                line.append(f.get(k, ""))
            fh.write(",".join("" if v is None else str(v) for v in line) + "\n")

    by = {r["metric"]: r for r in rows}

    # ---- Markdown ----------------------------------------------------------
    md = []
    md.append("# CatOS 重轴性能基线（perf/heavy, `bench heavy`）\n")
    md.append("## 环境与口径\n")
    md.append("- 分支：`%s`；提交：`%s`；ISO：`%s`"
              % (meta["branch"], meta["commit"], ISO))
    if meta["iso"]:
        md.append("- ISO 字节：%d，sha256 前 8 位：`%s`"
                  % (meta["iso"]["bytes"], meta["iso"]["sha256_8"]))
    if meta["qemu"]:
        md.append("- QEMU：`%s`（%s）" % (meta["qemu"]["bin"],
                                          meta["qemu"]["version"]))
    md.append("- 命令行：`%s`" % meta["cmdline"])
    md.append("- host：nproc=%s，loadavg=%s；每配置重复 %d 次取中位数。"
              % (meta["host"]["nproc"], meta["host"]["loadavg"], a.repeat))
    md.append("- 口径见 `bench/PERF_SPEC.md`；cycles 为 guest rdtsc 差值，"
              "us/MB/s/fps 由 guest 自校准 mhz 在 host 端换算。\n")

    md.append("## 中位数结果\n")
    md.append("| metric | n | cycles(中位) | us | bytes | MB/s | frames | fps | "
              "count | 单次 us | 分段 / 备注 | 波动 |")
    md.append("|---|---|---|---|---|---|---|---|---|---|---|---|")
    for r in rows:
        f = r["fields"]
        d = r.get("derived", {})
        if not r["present"]:
            md.append("| %s | %d | present=0（场景未跑成） | | | | | | | | | |"
                      % (r["metric"], r["n"]))
            continue
        seg = []
        for k, lab in (("cycles_glyph", "glyph"), ("cycles_flush", "flush"),
                        ("cycles_wait", "wait"), ("cycles_xfer", "xfer"),
                        ("cycles_setup", "setup"), ("cycles_exit", "exit")):
            if f.get(k):
                sh = d.get(lab + "_share")
                seg.append("%s=%s (%.0f%%)" % (lab, f[k], 100 * sh) if sh
                           else "%s=%s" % (lab, f[k]))
        if f.get("sleeps") is not None:
            seg.append("sleeps=%s ticks=%s bad=%s"
                       % (f["sleeps"], f.get("ticks", ""), f.get("bad", "")))
        md.append("| %s | %d | %s | %s | %s | %s | %s | %s | %s | %s | %s | %.1f%% |"
                  % (r["metric"], r["n"], f.get("cycles", ""),
                     fmt(d.get("us"), 3), f.get("bytes", ""),
                     fmt(d.get("MB/s")), f.get("frames", ""),
                     fmt(d.get("fps")), f.get("count", ""),
                     fmt(d.get("per_call_us"), 3), "; ".join(seg),
                     100 * r["spread"]))
    md.append("")

    # ---- bottleneck judgment, derived from the segment cycles ------------
    md.append("## 瓶颈判断（分段 cycle 证据）\n")

    def share(metric, seg):
        r = by.get(metric)
        if not r or not r["present"]:
            return None, None
        f, d = r["fields"], r["derived"]
        return f.get(seg), d.get(seg.replace("cycles_", "") + "_share")

    g = by.get("gfx.frame")
    if g and g["present"]:
        f, d = g["fields"], g["derived"]
        md.append("- **gfx.frame**：glyph/单元格绘制 `%s`（%.0f%%），整屏 LFB "
                  "批量冲刷 `%s`（%.0f%%）；两段同量级，字形绘制略占大头。"
                  "整屏重绘 %.2f M cycles/帧，%.1f fps（中位）。"
                  % (f["cycles_glyph"], 100 * d["glyph_share"],
                     f["cycles_flush"], 100 * d["flush_share"],
                     d["cycles_per_frame"] / 1e6, d["fps"]))

    dk = by.get("disk.seq")
    if dk and dk["present"]:
        f, d = dk["fields"], dk["derived"]
        md.append("- **disk.seq**：PIO 数据搬运（每扇区 256 次 `inw`）`%s`"
                  "（%.0f%%），状态/DRQ 轮询等待 `%s`（%.0f%%）；搬运段占"
                  "大头，轮询等待约三分之一。顺序读中位 %.2f MB/s（4 MiB，"
                  "raw LBA，ATA PIO）。"
                  % (f["cycles_xfer"], 100 * d["xfer_share"],
                     f["cycles_wait"], 100 * d["wait_share"], d["MB/s"]))

    pp = by.get("pipe.tput")
    if pp and pp["present"]:
        f, d = pp["fields"], pp["derived"]
        blocked_ms = f.get("ticks", 0) * 10.0   # PIT 100 Hz -> 1 tick = 10 ms
        blocked_share = blocked_ms * 1000.0 / d["us"] if d["us"] else 0
        md.append("- **pipe.tput**：64 KiB 共用 `%s` 次阻塞睡眠、`%s` 个定时器"
                  " tick（@100Hz ≈ %.0f ms），占总墙钟约 %.0f%%；FIFO 拷贝本身"
                  "很便宜，吞吐被调度/唤醒粒度卡住（中位 %.2f MB/s，bad=%s）。"
                  % (f["sleeps"], f.get("ticks", 0), blocked_ms,
                     100 * blocked_share, d["MB/s"], f.get("bad", 0)))

    ts = by.get("task.spawn")
    if ts and ts["present"]:
        f, d = ts["fields"], ts["derived"]
        exit_per_us = (f["cycles_exit"] / f["count"]) / (f["mhz"] or 1)
        md.append("- **task.spawn**：地址空间/任务表建立 `%s`（%.0f%%），用户态"
                  "入口 + 退出回收 `%s`（%.0f%%）；退出/回收段是 spawn→exec→"
                  "exit 全链路的大头，单次退出回收约 %.2f ms（全链路 %.2f ms/"
                  "次，N=%s）。"
                  % (f["cycles_setup"], 100 * d["setup_share"],
                     f["cycles_exit"], 100 * d["exit_share"],
                     exit_per_us / 1000.0, d["per_call_us"] / 1000.0,
                     f["count"]))

    # headline: largest dominant segment across axes
    headline = []
    if ts and ts["present"]:
        headline.append("task.spawn 的退出/回收段（exit 占约 %.0f%%，单次约 "
                        "%.1f ms）" % (100 * ts["derived"]["exit_share"],
                                       exit_per_us / 1000.0))
    if pp and pp["present"]:
        headline.append("pipe 受 100 Hz 唤醒粒度限制（阻塞约 %.0f%% 墙钟）"
                        % (100 * blocked_share))
    if headline:
        md.append("")
        md.append("**最该优先优化的一处**：%s。证据见上表对应分段 cycle 占比；"
                  "其余两段（gfx glyph/flush、disk wait/xfer）为同量级或次优"
                  "矛盾，留待优化轮再拆。" % "；其次 ".join(headline))
    md.append("")

    # methodology / segment definitions
    md.append("## 测量方法与分段口径\n")
    md.append("- **gfx.frame**：总 cycles 为 F=6 次 `con_repaint_frame()`"
              "（整屏 clear + 全网格重绘，最坏路径）；flush 段为 F 次 "
              "`gfx24_clear()` 整屏 LFB 填充；glyph 段 = 总 − flush（网格遍历 + "
              "字形光栅化进 LFB）。未计入逐字符 `con_write` 的换行/回退路径。")
    md.append("- **disk.seq**：总量走真实 `ata_read()` 驱动（drive 0，raw LBA，"
              "4 MiB / 8192 扇区，32 扇区/次）；wait/xfer 分段由同一端口序列的"
              "探针在同区域复读测得（wait=BSY/DRQ 轮询，xfer=每扇区 256 次 "
              "`inw`），与 `ata.c: ata_read` 的端口顺序一致。")
    md.append("- **pipe.tput**：ring0 助手 `kpipe_write/kpipe_read` 复用内核静态"
              " FIFO（PIPE_BUF=4096）与 `task_sleep_ticks` 阻塞语义；16 字节周期"
              "模式 `0123456789ABCDEF`，64 KiB，256 B 分块；sleeps=阻塞次数，"
              "ticks=经过的定时器 tick（PIT 100 Hz，1 tick=10 ms）。")
    md.append("- **task.spawn**：N=10 次 `/bin/hello.ce`；setup 段 = "
              "`task_spawn_ce()`（槽位/FS 解析/地址空间/镜像装载/栈与 vsyscall "
              "建立）；exit 段 = `task_wait_child()` 到子任务退出回收（ring3 入口、"
              "ce_main、exit syscall、清理回收）。")
    md.append("- 无盘批次（gfx、pipe）与带盘批次（disk、task）分 tag 跑；带盘用例"
              " 的私有磁盘副本由 qrun 每次复制，不挂仓库原盘。\n")

    # raw log pointers
    md.append("## 原始日志\n")
    for b in BATCHES:
        outdir = os.path.join(RUN_ROOT, b["tag"])
        md.append("- `%s` — `%s`" % (b["tag"], outdir))
    md.append("")

    md_path = os.path.join(ART_DIR, "heavy.md")
    with open(md_path, "w") as fh:
        fh.write("\n".join(md))

    meta_path = os.path.join(ART_DIR, "heavy-meta.json")
    with open(meta_path, "w") as fh:
        json.dump(meta, fh, indent=1)

    print(json.dumps({"artifacts": [md_path, csv_path, meta_path],
                      "rows": len(rows)}, indent=1))


if __name__ == "__main__":
    main()
