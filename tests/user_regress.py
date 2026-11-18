#!/usr/bin/env python3
"""user_regress.py -- on-metal regression for the user/capability pillar.

The security model (v2.13): a persistent /etc/passwd user database seeded
with root (uid 0, all capabilities) + user (uid 100), a per-user capability
bitmask enforced inside the kernel syscall layer, and a persistent unix
owner/group/other ACL (/etc/acl).  The boot session is root -- the initial
user.

  1.  format                 -> clean FAT32 volume
  2.  whoami                 -> root (initial session user)
  3.  lsuser                 -> seeded root + user accounts
  4.  useradd guest ...      -> new account, persisted, no capabilities
  5.  id guest               -> uid=101 caps=0x0
  6.  login guest            -> session switches; prompt shows guest
  7.  sudo / format          -> denied without CAP_USERADM / CAP_FSADMIN
  8.  perms (as guest)       -> uid=101, system writes EACCES, /home writes OK
  9.  curl (as guest)        -> net syscall denied: error -13 (CAP_NET)
 10.  su                     -> back to root without password
 11.  caps                   -> capability legend

The shell echoes and prints inline (no newline between prompt, command and
output), so every step asserts substrings of the whole screen after clear.

usage:
  python tests/user_regress.py [catos.iso] [--qemu PATH]
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qemu_driver import (boot as _qemu_boot, find_qemu,  # noqa: E402
                         kill_qemu, screen_text, sendkey, type_text,
                         wait_for_prompt, force_text)

REPO = os.path.dirname(HERE)
MONITOR_PORT = 4465                 # this driver's own port; never 4444
CRASH_MARKERS = ("kernel fault", "system halted")


def win_path(p):
    return os.path.abspath(p).replace("\\", "/")


def main():
    args = sys.argv[1:]
    iso = None
    if args and not args[0].startswith("--"):
        iso = args.pop(0)
    qemu = None
    if "--qemu" in args:
        i = args.index("--qemu")
        if i + 1 < len(args):
            qemu = args[i + 1]

    port = MONITOR_PORT
    if "--port" in args:
        i = args.index("--port")
        port = int(args[i + 1])
    iso = os.path.abspath(iso or os.path.join(REPO, "kernel_v25", "catos.iso"))
    qemu = find_qemu(qemu)
    if not qemu:
        print("qemu-system-i386 not found")
        return 2
    if not os.path.isfile(iso):
        print("ISO not found: %s" % iso)
        return 2

    disk = win_path(os.path.join(HERE, "qemu_disk_user.img"))
    if not os.path.exists(disk):
        with open(disk, "wb") as fh:
            fh.truncate(64 * 1024 * 1024)

    proc, s = _qemu_boot(qemu, iso, port, disk=disk)
    results = []

    def step(name, ok, note):
        results.append((name, ok))
        print("%-4s %-16s %s" % ("PASS" if ok else "FAIL", name, note))

    def do(line, wait=1.0):
        type_text(s, line)
        sendkey(s, "ret", wait)

    def clear():
        do("clear", 0.6)

    def screen():
        return screen_text(s)

    try:
        force_text(s)          # v2.56: boot defaults to gfx24; text suite
        if not wait_for_prompt(s, timeout=120.0):
            print("FATAL: shell prompt never appeared")
            return 3

        type_text(s, "format")
        sendkey(s, "ret", 1.0)
        sendkey(s, "y", 0.4)
        sendkey(s, "ret", 15.0)
        if not wait_for_prompt(s, timeout=90.0):
            print("FATAL: shell never returned after format")
            return 3
        step("format", True, "FAT32 volume ready")

        # 1b) capability probe as ROOT: net_init fails for the REAL reason
        # (no NIC in this QEMU: -1), not because of a capability gate
        clear()
        do("run /bin/captest.ce", 2.0)
        txt = screen()
        step("captest-root", "uid=0" in txt and "net_init=-1" in txt,
             "root passes the gate; NIC genuinely absent")

        # 2) initial session user is root (prompt shows root@catos)
        clear()
        do("whoami", 1.0)
        txt = screen()
        step("whoami-root", "root@catos" in txt and "whoami root" in txt,
             "session user = root")

        # 3) seeded accounts
        clear()
        do("lsuser", 1.2)
        txt = screen()
        ok = ("UID" in txt and "GID" in txt
              and "root" in txt and "user" in txt)
        step("lsuser", ok, "root + user seeded from /etc/passwd")

        # 4) create a zero-capability account (idempotent: /etc/passwd is
        # persistent, so a reused disk may already hold 'guest')
        do("userdel guest", 1.0)
        clear()
        do("useradd guest 101 101 0", 1.2)
        txt = screen()
        step("useradd", "user added" in txt, "persisted to /etc/passwd")

        # 5) the record is queryable
        clear()
        do("id guest", 1.2)
        txt = screen()
        ok = "uid=101" in txt and "gid=101" in txt and "caps=0x0" in txt
        step("id-guest", ok, "uid=101 gid=101 caps=0")

        # 6) switch the session to the guest
        clear()
        do("login guest", 1.2)
        txt = screen()
        step("login-guest", "logged in as guest" in txt, "session switched")
        clear()
        do("whoami", 1.0)
        txt = screen()
        step("session-guest", "guest@catos" in txt and "whoami guest" in txt,
             "prompt + whoami show guest")

        # 7) capability-gated shell commands must refuse
        clear()
        do("sudo run /bin/hello.ce", 1.2)
        txt = screen()
        step("sudo-denied", "sudo: permission denied" in txt,
             "CAP_USERADM required")
        clear()
        do("format", 1.5)
        sendkey(s, "esc", 0.3)          # in case a confirm prompt appeared
        sendkey(s, "n", 0.4)
        sendkey(s, "ret", 0.8)
        txt = screen()
        step("format-denied", "format: permission denied" in txt,
             "CAP_FSADMIN required")

        # 8) perms.ce as the guest: system files protected, /home writable
        clear()
        do("perms", 2.5)
        txt = screen()
        ok = ("uid=101" in txt and txt.count("EACCES") >= 2
              and txt.count("OK") >= 2)
        step("perms-guest", ok, "owner/group/other ACL enforced")

        # 9) privileged syscalls are denied by capability, not by absence
        # of hardware: the same probe returned net_init=-1 for root
        clear()
        do("run /bin/captest.ce", 2.0)
        txt = screen()
        ok = ("uid=101" in txt and "net_init=-13" in txt
              and "snd=-13" in txt)
        step("cap-denied", ok, "CAP_NET/CAP_AUDIO denial: -13")

        # 10) root may su back without a password
        clear()
        do("su", 1.2)
        txt = screen()
        step("su-root", "logged in as root" in txt, "root: no password")

        # 10b) v2.51 password hash: set a password, record is the v2 format
        #       ($S$<16 hex 64-bit salt>$<32 hex 128-bit digest>).
        clear()
        do("passwd user hunter2", 1.2)
        txt = screen()
        step("passwd-set", "password set" in txt, "root admin sets user password")
        clear()
        do("cat /etc/passwd", 1.2)
        txt = re.sub(r"\s+", "", screen())
        v2 = re.search(r"\$S\$[0-9a-f]{16}\$[0-9a-f]{32}", txt)
        step("pw-record-v2", v2 is not None,
             "record holds 64-bit salt + 128-bit digest")
        # 10c) wrong password denied; correct password logs in
        clear()
        do("login user wrongpass", 1.2)
        txt = screen()
        step("pw-wrong-denied", "incorrect password" in txt,
             "wrong password rejected")
        clear()
        do("login user hunter2", 1.2)
        txt = screen()
        step("pw-right-ok", "logged in as user" in txt,
             "correct password logs in")
        clear()
        do("whoami", 1.0)
        txt = screen()
        step("session-user-pw", "user@catos" in txt, "session = user")
        # back to root for the legend step
        clear()
        do("su", 1.2)

        # 11) capability legend
        clear()
        do("caps", 1.2)
        txt = screen()
        ok = ("0x0001 net" in txt and "0x0010 useradm" in txt
              and "caps=0x" in txt)
        step("caps", ok, "bit legend + session caps")

        low = screen().lower()
        if any(m in low for m in CRASH_MARKERS):
            step("no-crash", False, "kernel fault text seen")
        else:
            step("no-crash", True, "kernel survived the whole session")
    finally:
        kill_qemu(proc)

    fails = [n for n, ok in results if not ok]
    print("\nsteps=%d ok=%d fail=%d" % (len(results),
                                        len(results) - len(fails), len(fails)))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
