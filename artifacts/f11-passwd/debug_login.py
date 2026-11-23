#!/usr/bin/env python3
"""Debug: trace screen after `login` switch; check guest alive + text buffer."""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from f11_lib import (ISO, boot_guest, settle, run, clear, dump_text, shot,
                      stage_disk, kill_qemu, read_screen, screen_text)

PORT = 4482


def show(tag, sock):
    time.sleep(0.8)
    txt = read_screen(sock)
    print("---- %s (%d chars) ----" % (tag, len(txt)))
    print(txt)
    return txt


def main():
    disk = stage_disk("debug")
    proc, sock = boot_guest(ISO, disk, PORT, log="/tmp/f11_qemu_debug.log")
    try:
        settle(sock)
        run(sock, "useradd alice 101 101 0", 1.2)
        show("after useradd", sock)
        run(sock, "passwd alice AlicePass1", 1.2)
        show("after passwd", sock)
        run(sock, "login alice AlicePass1", 2.0)
        show("after login", sock)
        run(sock, "whoami", 1.0)
        show("whoami", sock)
        run(sock, "lsuser", 1.2)
        show("lsuser", sock)
        run(sock, "edit -c /etc/passwd", 1.5)
        show("edit passwd", sock)
        shot(sock, "debug_final")
    finally:
        kill_qemu(proc)


if __name__ == "__main__":
    main()
