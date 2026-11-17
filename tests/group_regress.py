#!/usr/bin/env python3
"""group_regress.py -- v2.53 supplementary groups regression.

Covers:
  v2.53-1  groupadd creates /etc/group entry and `groups` lists it
  v2.53-2  member user can read a group-owned file via supplementary group
  v2.53-3  non-member user is denied (mode 640, other = 0)
  v2.53-4  groupadd appends a second member; both can read
  v2.53-5  group db persisted: `groups` still lists the group after re-login
  v2.53-6  no fatal fault after the round-trip

usage:
  python tests/group_regress.py <catos.iso> [--qemu PATH] [--port N]
"""
import os, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qemu_driver import (boot, find_qemu, kill_qemu, screen_text, sendkey,
                         type_text, wait_for_prompt)

PORT = 4466


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
        # absorb boot-time PS/2 ghost keystrokes
        run(sock, "clear")

        # two test users, no capabilities
        run(sock, "useradd alice 101 101 0", 1.5)
        run(sock, "useradd bob 102 102 0", 1.5)

        # v2.53-1 create the group with alice as member
        t = run(sock, "groupadd devs alice", 1.5)
        r.check("groupadd-creates", "group 'devs' gid" in t, t)
        t = clean_run(sock, "groups")
        r.check("groups-lists", "devs (gid" in t and "alice" in t, t)

        # group-owned file: root:devs, mode 640 (group may read)
        run(sock, "mkdir /tmp/gdir", 1.0)
        run(sock, "hello > /tmp/gdir/secret.txt", 1.8)
        t = run(sock, "chown 0:100 /tmp/gdir/secret.txt", 1.2)
        r.check("chown-group-set", "owner updated" in t, t)
        t = run(sock, "chmod 640 /tmp/gdir/secret.txt", 1.2)
        r.check("chmod-640-set", "mode updated" in t, t)

        # v2.53-2 alice (member of devs) can read
        run(sock, "login alice", 1.5)
        t = clean_run(sock, "cat /tmp/gdir/secret.txt")
        r.check("member-reads-via-supgroup", "Hello from CE" in t, t)

        # v2.53-3 bob (not a member) is denied
        run(sock, "login bob", 1.5)
        t = clean_run(sock, "cat /tmp/gdir/secret.txt")
        r.check("nonmember-denied",
                "permission denied" in t and "Hello from CE" not in t, t)

        # v2.53-4 append bob, then bob can read too
        run(sock, "login root", 1.5)
        t = run(sock, "groupadd devs bob", 1.5)
        r.check("groupadd-appends", "group 'devs' gid" in t, t)
        run(sock, "login bob", 1.5)
        t = clean_run(sock, "cat /tmp/gdir/secret.txt")
        r.check("appended-member-reads", "Hello from CE" in t, t)

        # v2.53-5 persistence: back as root, groups still lists both members
        run(sock, "login root", 1.5)
        t = clean_run(sock, "groups")
        r.check("groups-persisted",
                "devs (gid" in t and "alice" in t and "bob" in t, t)

        # cleanup: drop the test users so other suites start clean
        run(sock, "userdel bob", 1.0)
        run(sock, "userdel alice", 1.0)
        # wipe the stale ACL entry (0,100,640) left by this run BEFORE rm:
        # /etc/acl is keyed by path and outlives the file, and the next run's
        # content-writer (uid 100) would hit it and keep the file empty.
        run(sock, "chown 0:0 /tmp/gdir/secret.txt", 1.0)
        run(sock, "chmod 644 /tmp/gdir/secret.txt", 1.0)
        run(sock, "rm /tmp/gdir/secret.txt", 1.0)
        run(sock, "rmdir /tmp/gdir", 1.0)

        # v2.53-6 no fatal fault
        t = clean_run(sock, "faultinfo")
        r.check("no-fatal-fault",
                "panic" not in t.lower() and "fatal" not in t.lower(), t)
    finally:
        kill_qemu(proc)

    print("\n%d checks, %d failed" % (r.n, len(r.fails)))
    if r.fails:
        print("FAILED: " + ", ".join(r.fails))
        sys.exit(1)
    print("group_regress: ALL PASS")


if __name__ == "__main__":
    main()
