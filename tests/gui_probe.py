#!/usr/bin/env python3
"""gui_probe.py -- M1/M2 GUI 自检探针驱动（复用 probes 基建的判定解析）。

流程：固定命令行起客人机 -> force_text 回文本控制台 -> 输入 `gui <subcmd>`
-> 串口落盘 -> 用 gui_parse 扫描 GUI: 打点与异常行 -> 按子命令断言。

用法：
    python3 tests/gui_probe.py idem
    python3 tests/gui_probe.py dirty
    python3 tests/gui_probe.py leak
    python3 tests/gui_probe.py geom [iso]

环境变量：
    PROBES_DIR  外部探针基建目录（含 gui_parse.py、gates/qemu_driver.py）
"""
import os
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)

PROBES_DIR = os.environ.get(
    "PROBES_DIR", "/home/user/work/artifacts/gui-blocks/probes")
sys.path.insert(0, os.path.join(PROBES_DIR, "gates"))
sys.path.insert(0, PROBES_DIR)

from qemu_driver import (find_qemu, kill_qemu, force_text,  # noqa: E402
                         sendkey, type_text, wait_for_prompt)
from gui_parse import parse_log  # noqa: E402

ART = os.path.join(REPO, "artifacts", "gui-blocks")
os.makedirs(ART, exist_ok=True)
ISO = os.path.join(REPO, "kernel_v25", "catos.iso")
PORT = 4471

# 子命令 -> (期望打点, 断言说明)
EXPECT = {
    "idem":  {"latest": {"match": "1"}, "flag": "GUI:idempotent=OK"},
    "dirty": {"latest": {"result": "1"}, "flag": None},
    "leak":  {"latest": {"leaked": "0"}, "flag": None},
    "geom":  {"latest": {}, "flag": None},
    "drag":  {"latest": {"result": "1"}, "flag": None},
    "hit":   {"latest": {"result": "1"}, "flag": None},
    "rate":  {"latest": {"result": "1"}, "flag": None},
}


def boot(qemu, iso, serial):
    return subprocess.Popen(
        [qemu, "-m", "256", "-smp", "1", "-cpu", "qemu32", "-vga", "vmware",
         "-no-reboot", "-net", "none", "-display", "none", "-boot", "order=d",
         "-cdrom", iso, "-serial", "file:" + serial,
         "-monitor", "tcp:127.0.0.1:%d,server,nowait" % PORT],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def main():
    sub = sys.argv[1] if len(sys.argv) > 1 else "idem"
    iso = sys.argv[2] if len(sys.argv) > 2 else ISO
    if sub not in EXPECT:
        print("FAIL: unknown subcmd %s" % sub)
        return 2

    qemu = find_qemu()
    if not qemu:
        print("FAIL: qemu-system-i386 not found")
        return 2

    serial = os.path.join(ART, "%s-serial.log" % sub)
    if os.path.exists(serial):
        os.remove(serial)

    proc = boot(qemu, iso, serial)
    sock = None
    try:
        for _ in range(40):
            if proc.poll() is not None:
                print("FAIL: qemu exited rc=%r" % proc.returncode)
                return 2
            try:
                sock = socket.create_connection(("127.0.0.1", PORT), timeout=5)
                break
            except OSError:
                time.sleep(2)
        if sock is None:
            print("FAIL: no monitor")
            return 2

        force_text(sock)
        if not wait_for_prompt(sock, timeout=120.0):
            print("FAIL: shell prompt never appeared")
            return 3

        type_text(sock, "gui " + sub)
        sendkey(sock, "ret", 1.0)
        time.sleep(4.0)
    finally:
        kill_qemu(proc)

    with open(serial, "rb") as fh:
        res = parse_log(fh.read().decode("utf-8", "replace"))

    s = res["summary"]
    print("== GUI records: %d ==" % s["gui_lines"])
    for k, n in sorted(s["keys"].items()):
        print("  %-14s x%-3d latest=%s" % (k, n, s["latest"][k]))
    print("== anomalies: %d ==" % len(res["anomalies"]))
    for a in res["anomalies"]:
        print("  L%-5d [%-12s] %s" % (a["line"], a["kind"], a["raw"]))

    ok = not res["anomalies"]
    exp = EXPECT[sub]
    for k, v in exp["latest"].items():
        if s["latest"].get(k) != v:
            print("FAIL: %s latest=%s want %s" % (k, s["latest"].get(k), v))
            ok = False
    if exp["flag"]:
        hit = any(exp["flag"] in r["raw"] for r in res["gui"])
        if not hit:
            print("FAIL: missing flag %s" % exp["flag"])
            ok = False

    print("RESULT=%s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
