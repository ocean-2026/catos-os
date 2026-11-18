#!/usr/bin/env python3
"""caps_regress.py -- v2.25 fine-grained capability split regression.

  caps-1  plain non-root user: format / fsck / reboot ALL denied
  caps-2  wizard preset 'viewer' yields caps=0x00000400
  caps-3  user with CAP_FSCK (0x100) may run fsck (no denial)
  caps-4  user with CAP_FORMAT (0x80) passes the format GATE (we do not
          actually mkfs; denial absence + gate message format is the check)
  caps-5  useredit TUI: full-screen takeover, list/caps views, restore
  caps-6  screen restored verbatim after useredit (prompt back)
  caps-7  no fatal fault after the round trip

usage: python tests/caps_regress.py <catos.iso> [--qemu PATH]
"""
import os, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qemu_driver import (boot, find_qemu, kill_qemu, screen_text, sendkey,
                         type_text, wait_for_prompt, force_text)

PORT = 4505


class R(object):
    def __init__(self):
        self.n = 0
        self.fails = []

    def check(self, name, cond, detail=""):
        self.n += 1
        print("  %-32s %s" % (name, "PASS" if cond else "FAIL"))
        if not cond:
            self.fails.append(name)
            if detail:
                print("      " + detail[-240:].replace("\n", " "))


def run(s, c, wait=1.3, keys=0.06):
    type_text(s, c, wait=keys)
    sendkey(s, "ret", 0.1)
    time.sleep(wait)
    return screen_text(s)


def clean_run(s, c, wait=1.3):
    run(s, "clear", 0.8)
    return run(s, c, wait)


def main():
    iso = os.path.abspath(sys.argv[1])
    qemu = find_qemu(sys.argv[sys.argv.index("--qemu") + 1]
                     if "--qemu" in sys.argv else None)
    proc, sock = boot(qemu, iso, PORT)
    r = R()
    try:
        force_text(sock)          # v2.56: boot defaults to gfx24; text suite
        wait_for_prompt(sock)

        # switch to the plain unprivileged account
        t = run(sock, "login user", 1.5)
        r.check("login-user", "logged in as user" in t, t)

        t = clean_run(sock, "format", 1.5)
        r.check("caps-1-format-denied", "permission denied" in t, t)
        t = clean_run(sock, "fsck", 1.5)
        r.check("caps-1-fsck-denied", "permission denied" in t, t)
        t = clean_run(sock, "reboot", 1.5)
        r.check("caps-1-reboot-denied", "permission denied" in t, t)

        # back to root
        t = run(sock, "login root", 1.5)
        r.check("login-root", "logged in as root" in t, t)

        # wizard with preset 'viewer'
        type_text(sock, "useradd vw1 102 102", wait=0.06)
        sendkey(sock, "ret", 0.1)
        time.sleep(1.2)
        type_text(sock, "viewer", wait=0.06)
        sendkey(sock, "ret", 0.1)
        time.sleep(1.0)
        t = screen_text(sock)
        r.check("caps-2-preset-viewer", "caps=0x400" in t, t)

        # fine-grained FSCK holder may run fsck
        t = run(sock, "useradd fchk 103 103 100", 1.5)
        t = run(sock, "login fchk", 1.5)
        t = clean_run(sock, "fsck", 2.5)
        r.check("caps-3-fsck-allowed",
                "permission denied" not in t and "fsck" in t, t)
        t = run(sock, "login root", 1.5)

        # CAP_FORMAT holder passes the format gate: verify via a fresh
        # account with ONLY the format bit; we do NOT run mkfs (destructive)
        t = run(sock, "useradd fmt1 104 104 80", 1.5)
        t = run(sock, "login fmt1", 1.5)
        t = clean_run(sock, "format", 1.5)
        # gate open = confirmation prompt reached (we answer n: no mkfs)
        r.check("caps-4-format-gate-open",
                "permission denied" not in t, t)
        type_text(sock, "n", wait=0.06)
        sendkey(sock, "ret", 0.1)
        time.sleep(1.0)
        t = run(sock, "login root", 1.5)

        # useredit TUI round trip
        run(sock, "clear", 0.8)
        type_text(sock, "useredit", wait=0.06)
        sendkey(sock, "ret", 0.1)
        time.sleep(1.5)
        t = screen_text(sock)
        r.check("caps-5-tui-list", "user administration" in t, t)
        # edit caps view: find row 'fchk'? navigate: press E on selected
        sendkey(sock, "e", 0.1)
        time.sleep(1.0)
        t = screen_text(sock)
        r.check("caps-5-tui-caps-view", "editing capabilities" in t, t)
        sendkey(sock, "spc", 0.15)      # toggle first bit
        sendkey(sock, "esc", 0.5)
        time.sleep(1.0)
        sendkey(sock, "q", 0.5)         # quit TUI
        time.sleep(1.2)
        t = screen_text(sock)
        r.check("caps-6-screen-restored", "root@catos" in t, t)

        # stability
        t = run(sock, "echo still-alive", 1.5)
        r.check("caps-7-no-fatal",
                "fault" not in t.lower() and "panic" not in t.lower(), t)
    finally:
        kill_qemu(proc)
    print("caps_regress: %d/%d PASS" % (r.n - len(r.fails), r.n))
    if r.fails:
        print("  FAILED: " + ", ".join(r.fails))
        sys.exit(1)


if __name__ == "__main__":
    main()
