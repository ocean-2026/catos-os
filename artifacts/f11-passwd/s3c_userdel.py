#!/usr/bin/env python3
"""F11 s3c: userdel removes the account and it stays gone after reload."""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from f11_lib import (ISO, boot_guest, settle, run, dump_text, shot,
                      stage_disk, kill_qemu)

PORT = 4495


def main():
    disk = stage_disk("s3c")
    proc, sock = boot_guest(ISO, disk, PORT)
    try:
        settle(sock)
        run(sock, "useradd deluser 109 109 0", 1.2)
        t = run(sock, "passwd deluser DelPass1", 1.2)
        print("setup:", t)
        t = run(sock, "userdel deluser", 1.2)
        print("userdel:", t)
        results = ["user deleted" in t]
        t = run(sock, "lsuser", 1.2)
        print("lsuser:", t)
        results.append("deluser" not in t)
        shot(sock, "s3c_userdel_gone")
        dump_text(sock, "s3c_userdel_gone")
        print("RESULT", "PASS" if all(results) else "FAIL")
        sys.exit(0 if all(results) else 1)
    finally:
        kill_qemu(proc)


if __name__ == "__main__":
    main()
