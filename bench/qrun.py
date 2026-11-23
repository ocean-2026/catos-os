#!/usr/bin/env python3
"""qrun.py -- one locked, CPU-pinned QEMU run of the CatOS bench build.

Every measurement in this task goes through this wrapper so that all runs
share ONE QEMU command line, one repeat protocol and one serial-capture
path.  See bench/PERF_SPEC.md for the conventions this implements.

Why the lock: the host has 2 cores and the guest runs under TCG.  Two
concurrent guests would steal cycles from each other and silently inflate
every cycle count, so runs are serialised on a machine-wide flock and each
guest is pinned to CPU 1 with `taskset`.

usage (single run, prints JSON):
  python3 bench/qrun.py --tag baseline --cmd "bench light" \
      --disk copy-of-catos_disk.img --repeat 3

usage (importable):
  from qrun import run_batch
  res = run_batch(tag="baseline", cmds=["bench light"], disk=disk_path)
"""
import argparse
import fcntl
import hashlib
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(REPO, "tests"))
from qemu_driver import cmd as hmp, sendkey, type_text  # noqa: E402

LOCK_PATH = "/home/user/work/catos-bench.lock"
RUN_ROOT = "/home/user/work/catos-bench-runs"
READY = "[boot] shell-ready"
END_MARK = "[perf] end "
CPU_PIN = "1"                 # taskset cpu list for the guest
DEFAULT_CPU = "qemu32"
DEFAULT_MEM = "256"


def _qemu_bin():
    for cand in ("qemu-system-i386", "qemu-system-x86_64"):
        p = shutil.which(cand)
        if p:
            return p
    raise SystemExit("qemu-system-i386 not found")


def qemu_argv(iso, disk, logpath, port, cpu=DEFAULT_CPU, mem=DEFAULT_MEM,
              extra=None):
    argv = ["taskset", "-c", CPU_PIN, _qemu_bin(),
            "-m", mem, "-smp", "1", "-cpu", cpu,
            "-no-reboot", "-net", "none", "-display", "none",
            "-cdrom", iso, "-boot", "order=d",
            "-serial", "file:" + logpath,
            "-monitor", "tcp:127.0.0.1:%d,server,nowait" % port]
    if disk:
        argv += ["-drive", "file=%s,format=raw,if=ide" % disk]
    if extra:
        argv += list(extra)
    return argv


def read_log(path):
    try:
        with open(path, "rb") as fh:
            return fh.read().decode("utf-8", "replace")
    except FileNotFoundError:
        return ""


def wait_for(path, needle, timeout):
    end = time.time() + timeout
    while time.time() < end:
        if needle in read_log(path):
            return True
        time.sleep(0.05)
    return False


def count_of(path, needle):
    return read_log(path).count(needle)


def one_run(tag, run_idx, iso, disk, outdir, cmds, cpu, mem, timeout,
            port_base, end_count=1):
    logpath = os.path.join(outdir, "%s.run%d.log" % (tag, run_idx))
    port = port_base + run_idx
    # fresh private disk copy per run: a scenario must never mutate the
    # reference image (kernel_v25/catos_disk.img is the user's own disk).
    disk_copy = None
    if disk:
        stagedir = tempfile.mkdtemp(prefix="catos_bench_%s_" % tag)
        disk_copy = os.path.join(stagedir, "disk.img")
        shutil.copy2(disk, disk_copy)
    argv = qemu_argv(iso, disk_copy, logpath, port, cpu, mem)
    meta = {
        "tag": tag, "run": run_idx, "argv": argv, "iso": iso,
        "iso_bytes": os.path.getsize(iso), "disk_src": disk, "log": logpath,
        "host": {"loadavg": open("/proc/loadavg").read().strip(),
                 "cpus": os.cpu_count()},
    }
    t0 = time.monotonic()
    proc = subprocess.Popen(argv, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    sock = None
    try:
        deadline = time.time() + 30
        while time.time() < deadline:
            if proc.poll() is not None:
                raise RuntimeError("qemu exited rc=%r" % proc.returncode)
            try:
                sock = socket.create_connection(("127.0.0.1", port), timeout=5)
                break
            except OSError:
                time.sleep(0.2)
        if sock is None:
            raise RuntimeError("no monitor on port %d" % port)
        if not wait_for(logpath, READY, timeout):
            raise RuntimeError("guest never printed %r; log tail: %s"
                               % (READY, read_log(logpath)[-400:]))
        meta["host_boot_ms"] = round((time.monotonic() - t0) * 1000, 1)
        # Type every line first (the guest queues the scancodes), then wait
        # for the scenario-completion sentinels.  Each `bench ...` line emits
        # exactly one `[perf] end` line, so end_count == number of bench
        # lines typed.
        for c in cmds:
            type_text(sock, c)
            sendkey(sock, "ret")
            time.sleep(1.0)
        deadline = time.time() + timeout
        while time.time() < deadline:
            if count_of(logpath, END_MARK) >= end_count:
                break
            if proc.poll() is not None:
                break
            # a BSOD halts the guest: surface it as a failure, never as a
            # zero metric.
            if "PANIC" in read_log(logpath):
                raise RuntimeError("guest faulted (PANIC in serial log)")
            time.sleep(0.1)
        else:
            raise RuntimeError("scenario did not finish; log tail: %s"
                               % read_log(logpath)[-400:])
        if count_of(logpath, END_MARK) < end_count:
            raise RuntimeError("guest stopped before the sentinel; log tail: %s"
                               % read_log(logpath)[-400:])
        meta["ok"] = True
    except Exception as exc:                       # noqa: BLE001
        meta["ok"] = False
        meta["error"] = str(exc)
        raise
    finally:
        try:
            if sock:
                sock.close()
        except Exception:                          # noqa: BLE001
            pass
        try:
            proc.terminate()
            proc.wait(timeout=5)
        except Exception:                          # noqa: BLE001
            try:
                proc.kill()
            except Exception:                      # noqa: BLE001
                pass
        if disk_copy:
            shutil.rmtree(os.path.dirname(disk_copy), ignore_errors=True)
    return meta


def run_batch(tag, cmds, iso=None, disk=None, repeat=3, outdir=None,
              cpu=DEFAULT_CPU, mem=DEFAULT_MEM, timeout=120,
              port_base=4600, end_count=1):
    iso = iso or os.path.join(REPO, "kernel_v25", "catos.iso")
    outdir = outdir or os.path.join(RUN_ROOT, tag)
    os.makedirs(outdir, exist_ok=True)
    metas = []
    with open(LOCK_PATH, "a") as lockfh:
        t_lock = time.time()
        fcntl.flock(lockfh, fcntl.LOCK_EX)
        try:
            print("bench lock acquired after %.1fs" % (time.time() - t_lock),
                  file=sys.stderr)
            for i in range(1, repeat + 1):
                metas.append(one_run(tag, i, iso, disk, outdir, cmds, cpu,
                                     mem, timeout, port_base, end_count))
                print("  run %d/%d ok" % (i, repeat), file=sys.stderr)
        finally:
            fcntl.flock(lockfh, fcntl.LOCK_UN)
    meta_path = os.path.join(outdir, "runs.json")
    with open(meta_path, "w") as fh:
        json.dump(metas, fh, indent=1)
    return {"tag": tag, "outdir": outdir, "runs": metas}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tag", required=True)
    ap.add_argument("--cmd", action="append", default=[],
                    help="shell command to run, in order (repeatable)")
    ap.add_argument("--iso", default=None)
    ap.add_argument("--disk", default=None,
                    help="reference disk image; copied fresh per run")
    ap.add_argument("--repeat", type=int, default=3)
    ap.add_argument("--outdir", default=None)
    ap.add_argument("--cpu", default=DEFAULT_CPU)
    ap.add_argument("--mem", default=DEFAULT_MEM)
    ap.add_argument("--timeout", type=int, default=120)
    ap.add_argument("--port-base", type=int, default=4600)
    ap.add_argument("--end-count", type=int, default=1,
                    help="number of `bench ...` lines that will emit [perf] end")
    a = ap.parse_args()
    cmds = a.cmd or ["bench light"]
    res = run_batch(a.tag, cmds, a.iso, a.disk, a.repeat, a.outdir,
                    a.cpu, a.mem, a.timeout, a.port_base, a.end_count)
    print(json.dumps(res, indent=1))


if __name__ == "__main__":
    main()
