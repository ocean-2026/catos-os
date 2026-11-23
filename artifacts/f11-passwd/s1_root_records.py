#!/usr/bin/env python3
"""Capture root view of persisted $C$ records (alice/bob, same password)."""
import os, sys, shutil
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from f11_lib import (ISO, boot_guest, settle, run, dump_text, shot,
                      kill_qemu)

PORT = 4494
SRC = "/tmp/catos_f11/f11_s1.img"
DST = "/tmp/catos_f11/f11_s1_rootview.img"


def main():
    shutil.copy2(SRC, DST)
    proc, sock = boot_guest(ISO, DST, PORT)
    try:
        settle(sock)
        t = run(sock, "edit -c /etc/passwd", 1.5)
        print(t)
        shot(sock, "s1_root_passwd_records")
        dump_text(sock, "s1_root_passwd_records")
    finally:
        kill_qemu(proc)


if __name__ == "__main__":
    main()
