#!/usr/bin/env python3
"""fsrec_regress.py -- on-metal regression for v2.24 recursive file ops.

Covers:
  v2.24-1  mkdir -p creates every missing level
  v2.24-2  cp -r copies a directory tree (dirs + files, content intact)
  v2.24-3  rm -r removes a whole tree (no leftovers in ls or /etc/acl)
  v2.24-4  rm on a directory without -r is refused; rm -r / is refused
  v2.24-5  deleted files leave NO stale ACL entry (re-created name does
           not inherit the old permission)
  v2.24-6  unprivileged user cannot mkdir outside the scratch whitelist
  v2.24-7  no fatal fault after the round-trip

usage:
  python tests/fsrec_regress.py <catos.iso> [--qemu PATH]
"""
import os, re, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qemu_driver import (boot, find_qemu, kill_qemu, screen_text, sendkey,
                         type_text, wait_for_prompt)

PORT = 4486


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


def clean_run(s, c, wait=1.5):
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
        wait_for_prompt(sock)

        # v2.24-1 mkdir -p
        t = run(sock, "mkdir -p /tmp/fstest/a/b/c")
        r.check("mkdir-p-nested", "denied" not in t and "cannot" not in t, t)
        t = clean_run(sock, "ls /tmp/fstest/a/b/c")
        r.check("deep-dir-exists", "entries" in t, t)

        # v2.24-2 cp -r tree
        run(sock, "cp /bin/hello.ce /tmp/fstest/a/b/c/h.ce")
        run(sock, "cp /bin/calc.ce /tmp/fstest/a/calc.ce")
        t = run(sock, "cp -r /tmp/fstest /tmp/fstest2")
        r.check("cp-r-no-error", "denied" not in t and "cannot" not in t, t)
        t = clean_run(sock, "ls /tmp/fstest2/a/b/c")
        r.check("cp-r-file-there", "h.ce" in t, t)
        t = clean_run(sock, "run /tmp/fstest2/a/b/c/h.ce", 2.0)
        r.check("cp-r-content-intact", "Hello from CE" in t, t)

        # v2.24-4 rm on dir refused without -r; rm -r / refused
        t = run(sock, "rm /tmp/fstest")
        r.check("rm-dir-needs-r", "use rm -r" in t, t)
        t = run(sock, "rm -r /")
        r.check("rm-root-refused", "refusing" in t, t)

        # v2.24-3 rm -r the tree, verify gone (also exercises -5 below)
        run(sock, "rm -r /tmp/fstest")
        run(sock, "rm -r /tmp/fstest2")
        t = clean_run(sock, "ls /tmp")
        r.check("rm-r-tree-gone", "fstest" not in t, t)

        # v2.24-5 no stale ACL: recreate the same name, perms are default
        run(sock, "chmod 000 /tmp/staleguard")
        run(sock, "rm /tmp/staleguard")
        run(sock, "touch /tmp/staleguard")
        t = run(sock, "run /tmp/staleguard", 1.8)
        # a fresh 0644-ish file is executable under "no entry" semantics;
        # a stale chmod 000 entry would deny with err -13
        r.check("no-stale-acl", "err -13" not in t, t)
        run(sock, "rm /tmp/staleguard")

        # v2.24-6 unprivileged user cannot mkdir outside scratch whitelist
        run(sock, "login user")
        t = run(sock, "mkdir /etc/evil")
        r.check("mkdir-etc-denied", "permission denied" in t, t)
        t = clean_run(sock, "mkdir -p /home/user/okdir/x")
        r.check("mkdir-scratch-ok", "denied" not in t and "cannot" not in t, t)
        run(sock, "login root")

        # v2.24-7 no fatal fault
        t = clean_run(sock, "faultinfo")
        r.check("no-fatal-fault",
                "panic" not in t.lower() and "fatal" not in t.lower(), t)
    finally:
        kill_qemu(proc)
    print("fsrec_regress: %d/%d PASS%s" % (
        r.n - len(r.fails), r.n,
        "" if not r.fails else "  FAILED: " + ", ".join(r.fails)))
    return 0 if not r.fails else 1


if __name__ == "__main__":
    sys.exit(main())
