#!/usr/bin/env python3
"""F11 scenario 1: v3 $C$ records, per-password salt, non-root read denial,
lsuser masking, root visibility.
"""
import os
import re
import sys
import time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from f11_lib import (ISO, boot_guest, settle, run, dump_text, shot,
                      stage_disk, kill_qemu, read_screen, screen_text)

PORT = 4481


def main():
    disk = stage_disk("s1")
    proc, sock = boot_guest(ISO, disk, PORT)
    results = []
    try:
        settle(sock)

        run(sock, "useradd alice 101 101 0", 1.2)
        t = run(sock, "passwd alice AlicePass1", 1.2)
        print("passwd alice:", t)
        results.append(("passwd-alice", "password set" in t, t))

        run(sock, "useradd bob 102 102 0", 1.2)
        t = run(sock, "passwd bob AlicePass1", 1.2)
        print("passwd bob:", t)
        results.append(("passwd-bob", "password set" in t, t))

        t = run(sock, "lsuser", 1.2)
        print("root lsuser:", t)
        results.append(("root-lsuser-tags", "$C$" in t, t))

        # switch to alice
        t = run(sock, "login alice AlicePass1", 2.0)
        print("login alice:", t)
        results.append(("login-alice-ok", "logged in as alice" in t, t))

        # non-root: read must be denied through the edit -c (cat) path
        t = run(sock, "edit -c /etc/passwd", 1.5)
        print("alice edit -c passwd:", t)
        results.append(("nonroot-passwd-denied", "permission denied" in t, t))
        shot(sock, "s1_nonroot_denied")
        dump_text(sock, "s1_nonroot_denied")

        t = run(sock, "lsuser", 1.2)
        print("alice lsuser:", t)
        # screen_text returns scrollback too, so judge only the segment after
        # the last `alice@catos:/$ lsuser` prompt (stale root output above it
        # still contains $C$ records).
        aseg = t[t.rfind("alice@catos:/$ lsuser"):]
        results.append(("nonroot-lsuser-mask",
                         "$C$" not in aseg and
                         re.search(r"0101\s+0101\s+\*\s+00000000\s+alice", aseg) is not None,
                         aseg))
        shot(sock, "s1_nonroot_lsuser_mask")
        dump_text(sock, "s1_nonroot_lsuser_mask")

        # back to root (root has no password)
        t = run(sock, "login root", 2.0)
        print("login root:", t)
        results.append(("login-root", "logged in as root" in t, t))

        t = run(sock, "edit -c /etc/passwd", 1.5)
        print("root edit -c passwd:", t)
        shot(sock, "s1_root_passwd_records")
        dump_text(sock, "s1_root_passwd_records")

        # long $C$ records wrap at the 80-col screen edge, so flatten the
        # segment after the command prompt before matching full digests.
        rseg = t[t.rfind("root@catos:/# edit -c /etc/passwd"):]
        flat = re.sub(r"\s+", "", rseg)
        digests = re.findall(r"\$C\$[0-9a-f]{16}\$[0-9a-f]{64}", flat)
        print("records:", digests)
        results.append(("root-sees-C-records", len(digests) >= 2, t))
        results.append(("salts-differ-same-pw",
                         len(digests) >= 2 and len(set(digests)) == len(digests),
                         "digests=%s" % digests))

        for name, ok, note in results:
            print("%-4s %-26s %s" % ("PASS" if ok else "FAIL", name, note[:160]))
        if any(not ok for _, ok, _ in results):
            sys.exit(1)
    finally:
        kill_qemu(proc)


if __name__ == "__main__":
    main()
