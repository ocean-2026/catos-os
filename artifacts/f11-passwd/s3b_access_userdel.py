#!/usr/bin/env python3
"""F11 s3b: non-root denial + mask, root read, userdel removal."""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from f11_lib import (ISO, boot_guest, settle, run, dump_text, shot,
                      stage_disk, kill_qemu)

PORT = 4493


def main():
    disk = stage_disk("s3b")
    proc, sock = boot_guest(ISO, disk, PORT)
    results = []
    try:
        settle(sock)
        run(sock, "useradd tmpuser 108 108 0", 1.2)
        t = run(sock, "passwd tmpuser TmpPass1", 1.2)
        results.append(("setup", "password set" in t, t))

        t = run(sock, "lsuser", 1.2)
        results.append(("root-lsuser", "tmpuser" in t, t))

        run(sock, "login tmpuser TmpPass1", 2.0)
        t = run(sock, "edit -c /etc/passwd", 1.5)
        results.append(("nonroot-edit-denied", "permission denied" in t, t))
        shot(sock, "s3b_nonroot_denied")
        dump_text(sock, "s3b_nonroot_denied")
        t = run(sock, "lsuser", 1.2)
        rows = [ln for ln in t.split("\n") if "tmpuser" in ln]
        mask_ok = bool(rows) and "*" in rows[-1] and "$C$" not in rows[-1]
        results.append(("nonroot-mask", mask_ok, t))

        run(sock, "login root", 2.0)
        t = run(sock, "edit -c /etc/passwd", 1.5)
        results.append(("root-readable", "tmpuser" in t, t))
        shot(sock, "s3b_root_readable")
        dump_text(sock, "s3b_root_readable")

        t = run(sock, "userdel tmpuser", 1.2)
        results.append(("userdel", "user deleted" in t, t))
        t = run(sock, "lsuser", 1.2)
        results.append(("userdel-gone", "tmpuser" not in t, t))

        for name, ok, note in results:
            print("%-4s %-22s %s" % ("PASS" if ok else "FAIL", name, str(note)[:160]))
        if any(not ok for _, ok, _ in results):
            sys.exit(1)
    finally:
        kill_qemu(proc)


if __name__ == "__main__":
    main()
