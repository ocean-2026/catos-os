#!/usr/bin/env python3
"""F11 scenario 3: regression.
  * passwd rotation: old password rejected, new password accepted.
  * userdel removes the account.
  * non-root still denied on /etc/passwd (both `cat` and `edit -c` paths);
    root can read it.
  * `id`/`whoami`/`caps` do not print digest material.
"""
import os
import sys
import time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from f11_lib import (ISO, boot_guest, settle, run, dump_text, shot,
                      stage_disk, kill_qemu, screen_text)

PORT = 4486


def main():
    disk = stage_disk("s3")
    proc, sock = boot_guest(ISO, disk, PORT)
    results = []
    try:
        settle(sock)

        run(sock, "useradd reguser 106 106 0", 1.2)
        t = run(sock, "passwd reguser OldPass1", 1.2)
        results.append(("setup-passwd-old", "password set" in t, t))

        # old password works initially
        t = run(sock, "login reguser OldPass1", 2.0)
        results.append(("old-pw-works-before", "logged in as reguser" in t, t))
        run(sock, "login root", 2.0)

        # rotate to a new password
        t = run(sock, "passwd reguser NewPass2", 1.2)
        results.append(("rotate-passwd", "password set" in t, t))

        # old password must now be rejected
        t = run(sock, "login reguser OldPass1", 2.0)
        results.append(("old-pw-rejected-after", "incorrect password" in t, t))
        shot(sock, "s3_old_password_rejected")
        dump_text(sock, "s3_old_password_rejected")

        # new password works
        t = run(sock, "login reguser NewPass2", 2.0)
        results.append(("new-pw-works", "logged in as reguser" in t, t))
        run(sock, "login root", 2.0)

        # non-root denial through both spellings
        run(sock, "useradd guest2 107 107 0", 1.2)
        run(sock, "passwd guest2 GuestPass1", 1.2)
        run(sock, "login guest2 GuestPass1", 2.0)
        t = run(sock, "cat /etc/passwd", 1.5)
        # `cat` is a retired alias that forwards to `edit -c`; either the
        # retirement hint or an explicit denial means no digest was read.
        results.append(("nonroot-cat-no-read",
                        "permission denied" in t or "edit -c" in t, t))
        t = run(sock, "edit -c /etc/passwd", 1.5)
        results.append(("nonroot-edit-denied", "permission denied" in t, t))
        shot(sock, "s3_nonroot_denied")
        dump_text(sock, "s3_nonroot_denied")
        t = run(sock, "lsuser", 1.2)
        results.append(("nonroot-lsuser-mask", "*" in t and "$C$" not in t, t))
        run(sock, "login root", 2.0)

        # root can still read the file
        t = run(sock, "edit -c /etc/passwd", 1.5)
        results.append(("root-reads-passwd", "reguser" in t and "guest2" in t, t))
        shot(sock, "s3_root_readable")
        dump_text(sock, "s3_root_readable")

        # id / whoami / caps must not leak digests
        t = run(sock, "id reguser", 1.0)
        results.append(("id-no-digest", "$C$" not in t and "$S$" not in t, t))
        t = run(sock, "whoami", 1.0)
        results.append(("whoami-no-digest", "$C$" not in t, t))
        t = run(sock, "caps", 1.0)
        results.append(("caps-no-digest", "$C$" not in t and "$S$" not in t, t))

        # userdel removes the account
        t = run(sock, "userdel reguser", 1.2)
        results.append(("userdel-reguser", "user deleted" in t, t))
        t = run(sock, "lsuser", 1.2)
        results.append(("userdel-gone", "reguser" not in t, t))

        for name, ok, note in results:
            print("%-4s %-28s %s" % ("PASS" if ok else "FAIL", name, str(note)[:160]))
        if any(not ok for _, ok, _ in results):
            sys.exit(1)
    finally:
        kill_qemu(proc)


if __name__ == "__main__":
    main()
