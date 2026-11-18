#!/usr/bin/env python3
"""recur_regress.py -- v2.50 recursive-op semantics regression.

Covers:
  v2.50-1  cp -r SRC EXISTING_DIR merges into EXISTING_DIR/SRC-basename
  v2.50-2  cp FILE EXISTING_DIR lands inside the dir (basename target)
  v2.50-3  cp carries the source ACL mode over (000 stays 000: run -> err -13)
  v2.50-4  rmdir removes an empty directory
  v2.50-5  rmdir refuses a non-empty directory
  v2.50-6  rmdir refuses a plain file
  v2.50-7  no fatal fault after the round-trip

usage:
  python tests/recur_regress.py <catos.iso> [--qemu PATH] [--port N]
"""
import os, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qemu_driver import (boot, find_qemu, kill_qemu, screen_text, sendkey,
                         type_text, wait_for_prompt, force_text)

PORT = 4469


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
        force_text(sock)
        wait_for_prompt(sock)

        # source tree: /tmp/rc/a/b with a file
        run(sock, "mkdir -p /tmp/rc/a/b")
        run(sock, "touch /tmp/rc/a/b/f.ce")

        # v2.50-1 merge into an existing directory
        run(sock, "mkdir /tmp/rcdst")
        t = run(sock, "cp -r /tmp/rc /tmp/rcdst")
        r.check("cp-r-merge-no-error", "cannot" not in t and "denied" not in t, t)
        t = clean_run(sock, "ls /tmp/rcdst/rc/a/b")
        r.check("cp-r-merge-nested", "f.ce" in t, t)

        # v2.50-2 plain file copied into an existing directory
        run(sock, "touch /tmp/rcfile")
        run(sock, "mkdir /tmp/rcdir")
        t = run(sock, "cp /tmp/rcfile /tmp/rcdir")
        r.check("cp-file-into-dir-no-error", "cannot" not in t and "denied" not in t, t)
        t = clean_run(sock, "ls /tmp/rcdir")
        r.check("cp-file-into-dir", "rcfile" in t, t)

        # v2.50-3 mode preservation: a 000 source keeps its no-exec mode on
        # the copy (spawn denies -13 before image parse).  The control copy
        # keeps the default exec bit and loads instead.
        run(sock, "cp /bin/hello.ce /tmp/modesrc")
        run(sock, "chmod 000 /tmp/modesrc")
        run(sock, "cp /tmp/modesrc /tmp/modedst")
        t = clean_run(sock, "run /tmp/modedst", 1.8)
        r.check("mode-preserved-000", "err -13" in t, t)
        run(sock, "cp /bin/hello.ce /tmp/modectrl")
        t = clean_run(sock, "run /tmp/modectrl", 1.8)
        r.check("control-keeps-exec-bit", "err -13" not in t, t)

        run(sock, "rm /tmp/modesrc")
        run(sock, "rm /tmp/modedst")
        run(sock, "rm /tmp/modectrl")

        # v2.50-4 rmdir empty dir
        run(sock, "mkdir /tmp/rd1")
        run(sock, "rmdir /tmp/rd1")
        t = clean_run(sock, "ls /tmp")
        r.check("rmdir-empty-gone", "rd1" not in t, t)

        # v2.50-5 rmdir non-empty refused
        t = run(sock, "rmdir /tmp/rc")
        r.check("rmdir-nonempty-refused", "not empty" in t, t)

        # v2.50-6 rmdir plain file refused
        t = run(sock, "rmdir /tmp/rcfile")
        r.check("rmdir-file-refused", "not a directory" in t, t)

        # cleanup
        run(sock, "rm -r /tmp/rc")
        run(sock, "rm -r /tmp/rcdst")
        run(sock, "rm -r /tmp/rcdir")

        # v2.50-7 no fatal fault
        t = clean_run(sock, "faultinfo")
        r.check("no-fatal-fault",
                "panic" not in t.lower() and "fatal" not in t.lower(), t)
    finally:
        kill_qemu(proc)
    print("recur_regress: %d/%d PASS%s" % (
        r.n - len(r.fails), r.n,
        "" if not r.fails else "  FAILED: " + ", ".join(r.fails)))
    return 0 if not r.fails else 1


if __name__ == "__main__":
    sys.exit(main())
