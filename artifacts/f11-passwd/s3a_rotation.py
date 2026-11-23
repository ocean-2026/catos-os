#!/usr/bin/env python3
"""F11 s3a: password rotation regression -- old password rejected after
rotation, new password accepted; capture both screens."""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from f11_lib import (ISO, boot_guest, settle, run, dump_text, shot,
                      stage_disk, kill_qemu, read_screen, screen_text)

PORT = 4492


def main():
    disk = stage_disk("s3a")
    proc, sock = boot_guest(ISO, disk, PORT)
    results = []
    try:
        settle(sock)
        run(sock, "useradd reguser 106 106 0", 1.2)
        t = run(sock, "passwd reguser OldPass1", 1.2)
        results.append(("setup-old", "password set" in t, t))

        t = run(sock, "login reguser OldPass1", 2.0)
        results.append(("old-works-before", "logged in as reguser" in t, t))
        run(sock, "login root", 2.0)

        t = run(sock, "passwd reguser NewPass2", 1.2)
        results.append(("rotate", "password set" in t, t))

        t = run(sock, "login reguser OldPass1", 2.0)
        results.append(("old-rejected", "incorrect password" in t, t))
        shot(sock, "s3_old_password_rejected")
        dump_text(sock, "s3_old_password_rejected")

        t = run(sock, "login reguser NewPass2", 2.0)
        results.append(("new-works", "logged in as reguser" in t, t))
        shot(sock, "s3_new_password_accepted")
        dump_text(sock, "s3_new_password_accepted")

        for name, ok, note in results:
            print("%-4s %-22s %s" % ("PASS" if ok else "FAIL", name, str(note)[:160]))
        if any(not ok for _, ok, _ in results):
            sys.exit(1)
    finally:
        kill_qemu(proc)


if __name__ == "__main__":
    main()
