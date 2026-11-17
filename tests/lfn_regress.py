#!/usr/bin/env python3
"""lfn_regress.py -- v2.48 LFN write-side robustness regression.

Covers:
  v2.48-1  long-name create / list / delete round trip (LFN run + alias)
  v2.48-2  FAT reserved characters are rejected ("invalid name"):
           : * ? < > \\ |  (control codes covered by the unit-level path)
  v2.48-3  alias collision: two long names sharing the first six stem
           characters coexist (REPOR~1 / REPOR~2 aliases)
  v2.48-4  multi-dot name goes through the LFN path and round-trips
  v2.48-5  mixed-case lossless name creates without fault
  v2.48-6  no fatal fault after the round-trip

usage:
  python tests/lfn_regress.py <catos.iso> [--qemu PATH] [--port N]
"""
import os, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qemu_driver import (boot, find_qemu, kill_qemu, screen_text, sendkey,
                         type_text, wait_for_prompt)

PORT = 4461


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


def run(s, c, wait=1.3):
    type_text(s, c)
    sendkey(s, "ret")
    time.sleep(wait)
    return screen_text(s)


def clean_run(s, c, wait=1.5):
    type_text(s, "clear")
    sendkey(s, "ret")
    time.sleep(0.8)
    return run(s, c, wait)


def main():
    iso = os.path.abspath(sys.argv[1])
    qemu = find_qemu(sys.argv[sys.argv.index("--qemu") + 1]
                     if "--qemu" in sys.argv else None)
    port = PORT
    if "--port" in sys.argv:
        i = sys.argv.index("--port")
        port = int(sys.argv[i + 1])
    proc, sock = boot(qemu, iso, port,
                      disk=os.path.abspath(os.path.join(
                          os.path.dirname(HERE),
                          "kernel_v25", "catos_disk.img")))
    r = R()
    try:
        wait_for_prompt(sock)

        # v2.48-1 long-name round trip
        run(sock, "touch /tmp/my-quarterly-finance-report-2026.txt")
        t = clean_run(sock, "ls /tmp")
        r.check("lfn-create-list", "my-quarterly-finance-report-2026.txt" in t, t)
        run(sock, "rm /tmp/my-quarterly-finance-report-2026.txt")
        t = clean_run(sock, "ls /tmp")
        r.check("lfn-delete-gone",
                "my-quarterly-finance-report-2026.txt" not in t, t)

        # v2.48-2 reserved characters rejected
        for bad in ["a:b.txt", "a*b.txt", "a?b.txt", "a<b.txt",
                    "a>b.txt", "a\\b.txt", "a|b.txt"]:
            t = run(sock, "touch /tmp/%s" % bad)
            r.check("illegal-%s" % bad.replace("\\", "bs"),
                    "invalid name" in t, t)

        # v2.48-3 alias collision: same six-char stem, two long names
        run(sock, "touch /tmp/report-alpha.md")
        run(sock, "touch /tmp/report-beta.md")
        t = clean_run(sock, "ls /tmp")
        r.check("alias-collision-both",
                "report-alpha.md" in t and "report-beta.md" in t, t)
        run(sock, "rm /tmp/report-alpha.md")
        run(sock, "rm /tmp/report-beta.md")

        # v2.48-4 multi-dot name through the LFN path
        run(sock, "touch /tmp/a.b.c.dots")
        t = clean_run(sock, "ls /tmp")
        r.check("multi-dot-roundtrip", "a.b.c.dots" in t, t)
        run(sock, "rm /tmp/a.b.c.dots")

        # v2.48-5 mixed-case names: lossless 8.3 shows lowercased; the
        # LFN run keeps the original case on read-back.
        run(sock, "touch /tmp/ReadMe.Txt")
        t = clean_run(sock, "ls /tmp")
        r.check("mixedcase-83-created", "readme.txt" in t, t)
        run(sock, "rm /tmp/ReadMe.Txt")
        run(sock, "touch /tmp/ReadMe.Longnote")
        t = clean_run(sock, "ls /tmp")
        r.check("mixedcase-lfn-keeps-case", "ReadMe.Longnote" in t, t)
        run(sock, "rm /tmp/ReadMe.Longnote")

        # v2.48-6 no fatal fault
        t = clean_run(sock, "faultinfo")
        r.check("no-fatal-fault",
                "panic" not in t.lower() and "fatal" not in t.lower(), t)
    finally:
        kill_qemu(proc)
    print("lfn_regress: %d/%d PASS%s" % (
        r.n - len(r.fails), r.n,
        "" if not r.fails else "  FAILED: " + ", ".join(r.fails)))
    return 0 if not r.fails else 1


if __name__ == "__main__":
    sys.exit(main())
