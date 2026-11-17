#!/usr/bin/env python3
"""daemon_regress.py -- v2.54 lightweight daemon primitives regression.

Covers:
  dm-1  `daemon memlog ...` announces a detached service tid
  dm-2  `jobs` lists the daemon with the `D` marker and a live state
  dm-3  `log TID` drains the captured watermark ring (heap=0x sample)
  dm-4  `kill TID` reaps the daemon; `jobs` no longer lists it
  dm-5  no fatal fault after the round-trip

The daemon's event loop sleeps on the timer wake source (delay), which is
one of the three existing blocking wake sources (timer / child-exit / key).

usage:
  python tests/daemon_regress.py <catos.iso> [--qemu PATH] [--port N]
"""
import os, re, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qemu_driver import (boot, find_qemu, kill_qemu, screen_text, sendkey,
                         type_text, wait_for_prompt)

PORT = 4467


class R(object):
    def __init__(self):
        self.n = 0
        self.fails = []

    def check(self, name, cond, detail=""):
        self.n += 1
        ok = bool(cond)
        print("  %-34s %s" % (name, "PASS" if ok else "FAIL"))
        if not ok:
            self.fails.append(name)
            if detail:
                print("      " + detail[-240:].replace("\n", " "))


def run(s, c, wait=1.5):
    type_text(s, c)
    sendkey(s, "ret")
    time.sleep(wait)
    return screen_text(s)


def announce_tid(t):
    m = re.search(r"daemon tid (\d+)", t)
    return int(m.group(1)) if m else 0


def main():
    qemu = find_qemu(sys.argv[sys.argv.index("--qemu") + 1]
                     if "--qemu" in sys.argv else None)
    iso = os.path.abspath(sys.argv[1])
    port = PORT
    if "--port" in sys.argv:
        port = int(sys.argv[sys.argv.index("--port") + 1])
    proc, sock = boot(qemu, iso, port,
                      disk=os.path.abspath(os.path.join(
                          os.path.dirname(HERE),
                          "kernel_v25", "catos_disk.img")))
    r = R()
    try:
        wait_for_prompt(sock)
        # PS/2 ghost-keystroke settle (docs/history.md): discard-type once and
        # wait for the controller to settle before the first real assertion.
        run(sock, "clear", 2.5)

        # dm-1: detach the watermark logger as a daemon.  60x500ms keeps the
        # service alive across the whole probe (a short-lived daemon would be
        # reaped before dm-3/dm-4 can observe it).
        t = run(sock, "daemon memlog 60 500", 2.0)
        tid = announce_tid(t)
        r.check("daemon-announce-tid", tid > 0, t)

        # dm-2: jobs lists it with the D marker and a live state
        t = run(sock, "jobs", 1.5)
        r.check("jobs-lists-daemon",
                "memlog" in t and "D" in t and
                ("run" in t or "sleep" in t), t)

        # dm-3: captured ring carries a heap watermark sample
        time.sleep(2.0)
        t = run(sock, "log %d" % tid, 1.5)
        r.check("log-watermark-sample",
                "memlog" in t and "heap=0x" in t, t)

        # dm-4: kill reaps the detached service (clear first: dm-3's log
        # output still contains "memlog" and would pollute a full-screen scan)
        run(sock, "kill %d" % tid, 1.5)
        run(sock, "clear", 0.8)
        t = run(sock, "jobs", 1.5)
        r.check("kill-reaps-daemon", "memlog" not in t, t)

        # dm-5: no fatal fault
        t = run(sock, "faultinfo", 1.5)
        r.check("no-fatal-fault",
                "panic" not in t.lower() and "fatal" not in t.lower(), t)
    finally:
        kill_qemu(proc)

    print("\n%d checks, %d failed" % (r.n, len(r.fails)))
    if r.fails:
        print("FAILED: " + ", ".join(r.fails))
        sys.exit(1)
    print("daemon_regress: ALL PASS")


if __name__ == "__main__":
    sys.exit(main())
