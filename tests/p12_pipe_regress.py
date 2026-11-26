#!/usr/bin/env python3
"""p12_pipe_regress.py -- H10 shell `A | B` pipeline regression (v2.82 P1-2).

Covers the baseline defect: shell_exec parsed the redirect/pipe marker once
up front (shell_line_noninteractive) and truncated arg at the `|`; the
external-command branch and `cmd_run` then re-parsed that truncated string,
so their local pipe_to was NULL and the right-hand consumer was never wired
-- the producer wrote to the console and no `SINK got N lines` line appeared.

Scenarios (text mode, on-metal):
  1. hello.ce (no pipe)            -- baseline console output.
  2. hello.ce | sink_ce.ce         -- producer stdout goes to the pipe; the
                                      consumer reports `SINK got 4 lines`;
                                      producer text must NOT leak to console.
  3. hello.ce | sink_ce.ce twice  -- second pipeline reuses pipe slots cleanly.
  4. ls /bin | sink_ce.ce         -- builtin left side: output matches a plain
                                      `ls /bin`; no consumer noise appears.

Usage:
  python tests/p12_pipe_regress.py [catos.iso] [--qemu PATH] [--shot-dir DIR]
"""
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qemu_driver import (boot, find_qemu, force_text, kill_qemu,  # noqa: E402
                         read_screen, screen_text, sendkey, type_text,
                         wait_for_prompt)

REPO = os.path.dirname(HERE)
MONITOR_PORT = 4472
SHOT_DIR = os.path.join(REPO, "artifacts", "v282", "p1-2")


def shoot(s, name):
    os.makedirs(SHOT_DIR, exist_ok=True)
    ppm = os.path.join(SHOT_DIR, name + ".ppm")
    png = os.path.join(SHOT_DIR, name + ".png")
    if os.path.exists(ppm):
        os.remove(ppm)
    from qemu_driver import cmd
    cmd(s, "screendump %s" % ppm.replace("\\", "/"), 1.0)
    for _ in range(10):
        if os.path.exists(ppm) and os.path.getsize(ppm) > 0:
            break
        time.sleep(0.3)
    if os.path.exists(ppm):
        subprocess.run(["convert", ppm, png], check=False)
        os.remove(ppm)
    return png


def main(argv):
    iso = os.path.join(REPO, "kernel_v25", "catos.iso")
    qemu = None
    shot_dir = None
    args = list(argv)
    while args:
        a = args.pop(0)
        if a == "--qemu":
            qemu = args.pop(0)
        elif a == "--shot-dir":
            shot_dir = args.pop(0)
        elif not a.startswith("-"):
            iso = os.path.abspath(a)
    global SHOT_DIR
    if shot_dir:
        SHOT_DIR = shot_dir
    qemu = find_qemu(qemu or "/home/user/bin/qemu-system-i386")
    if not qemu or not os.path.isfile(iso):
        print("p12: need QEMU and a built ISO (got %r / %r)" % (qemu, iso))
        return 2

    disk = os.path.join(HERE, "qemu_disk_p12.img")
    if not os.path.exists(disk):
        with open(disk, "wb") as fh:
            fh.truncate(64 * 1024 * 1024)

    proc, s = boot(qemu, iso, MONITOR_PORT, disk=disk)
    force_text(s)
    results = []

    def step(name, ok, note):
        results.append((name, ok))
        print("%-4s %-28s %s" % ("PASS" if ok else "FAIL", name, note))

    def run(line, wait=1.5):
        type_text(s, line)
        sendkey(s, "ret", wait)
        time.sleep(0.6)
        return read_screen(s)

    try:
        if not wait_for_prompt(s, timeout=90.0):
            print("FATAL: shell prompt never appeared")
            return 3
        type_text(s, "format"); sendkey(s, "ret", 1.0)
        sendkey(s, "y", 0.4); sendkey(s, "ret", 15.0)
        if not wait_for_prompt(s, timeout=90.0):
            print("FATAL: no prompt after format")
            return 3

        # 1) plain hello.ce
        run("clear", 0.8)
        plain = run("hello.ce", 3.0)
        shoot(s, "01-hello-plain")
        step("plain hello prints body",
             "Hello from CE!" in plain and "alloc test: abcdefghij" in plain,
             "body lines present")

        # 2) hello | sink
        run("clear", 0.8)
        piped = run("hello.ce | sink_ce.ce", 4.0)
        shoot(s, "02-hello-pipe-sink")
        sink_line = [ln.strip() for ln in piped.splitlines() if "SINK got" in ln]
        step("pipeline reports SINK line", len(sink_line) == 1,
             "sink=%r" % (sink_line,))
        step("producer stdout not leaked to console",
             "Hello from CE!" not in piped,
             "producer text went to the pipe")
        step("consumer counted 4 lines",
             bool(sink_line) and sink_line[0].endswith("4 lines"),
             sink_line[0] if sink_line else "no sink line")

        # 3) second pipeline run -- slot reuse must stay clean
        run("clear", 0.8)
        again = run("hello.ce | sink_ce.ce", 4.0)
        shoot(s, "03-hello-pipe-sink-again")
        again_sink = [ln.strip() for ln in again.splitlines() if "SINK got" in ln]
        step("second pipeline reports SINK line", len(again_sink) == 1,
             "sink=%r" % (again_sink,))

        # 4) builtin left side: piped ls must match plain ls
        run("clear", 0.8)
        ls_plain = run("ls /bin", 2.5)
        shoot(s, "04-ls-plain")
        run("clear", 0.8)
        ls_pipe = run("ls /bin | sink_ce.ce", 2.5)
        shoot(s, "05-ls-pipe-builtin-left")
        plain_entries = [ln.strip() for ln in ls_plain.splitlines() if ln.strip()]
        pipe_entries = [ln.strip() for ln in ls_pipe.splitlines() if ln.strip()]
        step("piped ls matches plain ls listing",
             plain_entries == pipe_entries,
             "plain=%d lines piped=%d lines" % (len(plain_entries),
                                                 len(pipe_entries)))
        step("builtin-left pipe spawns no consumer noise",
             not any("SINK got" in ln for ln in pipe_entries),
             "no SINK line on builtin left side")
    finally:
        kill_qemu(proc)

    fails = [n for n, ok in results if not ok]
    print("\nsteps=%d ok=%d fail=%d" % (len(results),
                                        len(results) - len(fails), len(fails)))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
