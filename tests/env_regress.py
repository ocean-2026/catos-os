#!/usr/bin/env python3
"""env_regress.py -- on-metal regression for v2.23 shell environment vars.

Covers:
  v2.23-1  export K=V lists in `env`, persisted in /etc/env
  v2.23-2  $K / ${K} expansion in typed lines (incl. spawn path)
  v2.23-3  unknown $K expands to empty, $$ stays literal
  v2.23-4  export K= deletes; unset K deletes
  v2.23-5  $PATH drives bare-NAME program lookup
  v2.23-6  no fatal fault after the round-trip

usage:
  python tests/env_regress.py <catos.iso> [--qemu PATH]
"""
import os, re, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qemu_driver import (boot, find_qemu, kill_qemu, screen_text, sendkey,
                         type_text, wait_for_prompt, force_text)

PORT = 4483


class R(object):
    def __init__(self):
        self.n = 0
        self.fails = []

    def check(self, name, cond, detail=""):
        self.n += 1
        ok = bool(cond)
        print("  %-32s %s" % (name, "PASS" if ok else "FAIL"))
        if not ok:
            self.fails.append(name)
            if detail:
                print("      " + detail[-240:].replace("\n", " "))


def run(s, c, wait=1.3):
    type_text(s, c)
    sendkey(s, "ret")
    time.sleep(wait)
    return screen_text(s)


def clean_run(s, c, wait=1.3):
    type_text(s, "clear")
    sendkey(s, "ret")
    time.sleep(0.8)
    return run(s, c, wait)


def main():
    iso = os.path.abspath(sys.argv[1])
    qemu = find_qemu(sys.argv[sys.argv.index("--qemu") + 1]
                     if "--qemu" in sys.argv else None)
    proc, sock = boot(qemu, iso, PORT,
                      disk=os.path.abspath(os.path.join(
                          os.path.dirname(HERE),
                          "kernel_v25", "catos_disk.img")))
    r = R()
    try:
        force_text(sock)          # v2.56: boot defaults to gfx24; text suite
        wait_for_prompt(sock)

        # v2.23-1 set + list
        t = run(sock, "export WOOK=helloval")
        r.check("export-set", "export: WOOK=helloval" in t, t)
        t = clean_run(sock, "env")
        r.check("env-lists", "WOOK=helloval" in t and "PATH=/bin" in t, t)
        t = clean_run(sock, "cat /etc/env")
        r.check("persisted-etc-env", "WOOK=helloval" in t, t)

        # v2.23-2 expansion on the spawn path: bare NAME with $VAR in the
        # argument must reach the app; `run $WOOK` proves the arg expanded
        t = clean_run(sock, "run $WOOK", 2.0)
        r.check("expand-spawn-path", "cannot load 'helloval" in t, t)

        # v2.23-3 unknown var -> empty; $$ -> literal
        t = clean_run(sock, "run $NOPE", 2.0)
        r.check("unknown-var-empty", "cannot load ''" in t or "usage: run" in t, t)
        t = clean_run(sock, "run $$x", 2.0)
        r.check("dollar-literal", "cannot load '$x" in t, t)

        # v2.23-4 delete via export K= and unset
        t = run(sock, "export WOOK=")
        r.check("export-empty-deletes", "unset: WOOK" in t, t)
        run(sock, "export TMPV=1")
        t = run(sock, "unset TMPV")
        r.check("unset-deletes", "unset: TMPV" in t, t)
        t = clean_run(sock, "env")
        r.check("deleted-gone", "WOOK=" not in t and "TMPV=" not in t, t)

        # v2.23-5 $PATH drives bare-NAME lookup
        t = clean_run(sock, "hello", 2.0)
        r.check("path-default-finds", "Hello from CE" in t, t)
        run(sock, "export PATH=/etc")
        t = clean_run(sock, "hello", 2.0)
        r.check("path-redirect-fails", "Hello from CE" not in t, t)
        run(sock, "export PATH=/bin")
        t = clean_run(sock, "hello", 2.0)
        r.check("path-restore-finds", "Hello from CE" in t, t)

        # v2.23-6 no fatal fault
        t = clean_run(sock, "faultinfo")
        r.check("no-fatal-fault",
                "panic" not in t.lower() and "fatal" not in t.lower(), t)
    finally:
        kill_qemu(proc)
    print("env_regress: %d/%d PASS%s" % (
        r.n - len(r.fails), r.n,
        "" if not r.fails else "  FAILED: " + ", ".join(r.fails)))
    return 0 if not r.fails else 1


if __name__ == "__main__":
    sys.exit(main())
