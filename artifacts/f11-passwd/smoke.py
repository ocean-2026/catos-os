#!/usr/bin/env python3
"""F11 smoke: ISO boots to shell, no panic; report session user + lsuser."""
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from f11_lib import (ISO, boot_guest, settle, run, clear, dump_text, shot,
                      stage_disk, kill_qemu)

PORT = 4480


def main():
    disk = stage_disk("smoke")
    proc, sock = boot_guest(ISO, disk, PORT)
    try:
        settle(sock)
        clear(sock)
        t = run(sock, "whoami", 1.0)
        print("whoami:", t)
        t = run(sock, "lsuser", 1.2)
        print("lsuser:", t)
        dump_text(sock, "smoke_lsuser")
        shot(sock, "smoke_boot")
        print("SMOKE OK")
    finally:
        kill_qemu(proc)


if __name__ == "__main__":
    main()
