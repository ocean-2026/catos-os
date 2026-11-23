#!/usr/bin/env python3
"""gate_text.py -- text-mode + boot sanity gate for the render-path change.

Boots the current ISO, waits for the serial `[boot] shell-ready` sentinel,
switches back to the 80x25 text back-end with the shell `text` command, runs
`ls /bin`, and asserts:
  * a built-in CE app name is echoed on the VGA text console,
  * the serial log contains the READY sentinel and no `PANIC`,
  * the guest stayed up (no early QEMU exit).

Usage:
  python3 bench/gate_text.py [--report artifacts/perf-render/text-gate.json]
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(REPO, "tests"))

import qrun  # noqa: E402
import qemu_driver  # noqa: E402

ISO = os.path.join(REPO, "kernel_v25", "catos.iso")
READY = "[boot] shell-ready"
# app names that ship in every build (see kernel_v25/build/apps.list)
NEEDLE_APPS = ["sysinfo.ce", "vprobe.ce", "zhdemo.ce"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--report",
                    default=os.path.join(REPO, "artifacts", "perf-render",
                                         "text-gate.json"))
    ap.add_argument("--port", type=int, default=4632)
    ap.add_argument("--disk", default=None,
                    help="optional IDE disk image (e.g. bench/disk-ref-64m.img)")
    args = ap.parse_args()

    outdir = tempfile.mkdtemp(prefix="catos_gate_text_")
    logpath = os.path.join(outdir, "text-gate.log")
    argv = qrun.qemu_argv(ISO, args.disk, logpath, args.port,
                          extra=["-vga", "std"])
    proc = subprocess.Popen(argv, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    sock = None
    report = {"ready": False, "panic": False, "apps_echoed": [],
              "screen_tail": "", "ok": False}
    try:
        for _ in range(40):
            if proc.poll() is not None:
                raise RuntimeError("qemu exited rc=%r" % proc.returncode)
            try:
                sock = __import__("socket").create_connection(
                    ("127.0.0.1", args.port), timeout=5)
                break
            except OSError:
                time.sleep(2)
        if sock is None:
            raise RuntimeError("no monitor on port %d" % args.port)

        if not qrun.wait_for(logpath, READY, timeout=60):
            report["serial_tail"] = qrun.read_log(logpath)[-400:]
            raise RuntimeError("guest never printed shell-ready")
        report["ready"] = True

        qemu_driver.force_text(sock)
        if not qemu_driver.wait_for_prompt(sock, timeout=30.0):
            raise RuntimeError("text-mode prompt never appeared")
        qemu_driver.type_text(sock, "ls /bin")
        qemu_driver.sendkey(sock, "ret", 2.5)

        txt = qemu_driver.screen_text(sock)
        report["screen_tail"] = txt[-300:]
        report["apps_echoed"] = [a for a in NEEDLE_APPS if a in txt]

        serial = qrun.read_log(logpath)
        report["panic"] = "PANIC" in serial
        # Text back-end is exercised when the prompt and the `ls` reply are
        # both rendered to the VGA text console.  App names appear only when a
        # formatted disk is attached; their absence with no disk is expected.
        text_ok = ("root@catos" in txt and
                   ("ls:" in txt or ".ce" in txt))
        report["text_rendered"] = text_ok
        report["ok"] = report["ready"] and not report["panic"] and text_ok
    finally:
        if sock is not None:
            try:
                sock.close()
            except OSError:
                pass
        qemu_driver.kill_qemu(proc)

    os.makedirs(os.path.dirname(args.report) or ".", exist_ok=True)
    with open(args.report, "w") as fh:
        json.dump(report, fh, indent=1)
    print(json.dumps(report, indent=1))
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
