#!/usr/bin/env python3
"""Debug: login alice -> login root, inspect screen buffer + PPM each step."""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from f11_lib import (ISO, boot_guest, settle, run, shot, stage_disk,
                      kill_qemu, read_screen)

PORT = 4483


def show(tag, sock):
    time.sleep(1.5)
    txt = read_screen(sock)
    print("---- %s (%d chars) ----" % (tag, len(txt)))
    print(txt)
    shot(sock, "debug2_" + tag.replace(" ", "_"))
    return txt


def main():
    disk = stage_disk("debug2")
    proc, sock = boot_guest(ISO, disk, PORT, log="/tmp/f11_qemu_debug2.log")
    try:
        settle(sock)
        run(sock, "useradd alice 101 101 0", 1.2)
        run(sock, "passwd alice AlicePass1", 1.2)
        run(sock, "login alice AlicePass1", 2.0)
        show("alice-whoami", sock)
        run(sock, "login root", 2.0)
        show("root-after-relogin", sock)
        run(sock, "whoami", 1.0)
        show("root-whoami", sock)
        run(sock, "lsuser", 1.2)
        show("root-lsuser", sock)
        run(sock, "edit -c /etc/passwd", 1.5)
        show("root-passwd-file", sock)
    finally:
        kill_qemu(proc)
        print("== qemu log tail ==")
        try:
            print(open("/tmp/f11_qemu_debug2.log").read()[-2000:])
        except Exception as e:
            print("no log", e)


if __name__ == "__main__":
    main()
