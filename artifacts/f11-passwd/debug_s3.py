#!/usr/bin/env python3
"""Debug s3: passwd rotation + relogin, trace screen after each step."""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from f11_lib import (ISO, boot_guest, settle, run, shot, stage_disk,
                      kill_qemu, read_screen)

PORT = 4487


def show(tag, sock):
    time.sleep(1.2)
    txt = read_screen(sock)
    print("---- %s (%d) ----" % (tag, len(txt)))
    print(txt)
    shot(sock, "debug3_" + tag.replace(" ", "_"))


def main():
    disk = stage_disk("debug3")
    proc, sock = boot_guest(ISO, disk, PORT, log="/tmp/f11_qemu_debug3.log")
    try:
        settle(sock)
        run(sock, "useradd reguser 106 106 0", 1.2); show("1-useradd", sock)
        run(sock, "passwd reguser OldPass1", 1.2); show("2-passwd-old", sock)
        run(sock, "login reguser OldPass1", 2.0); show("3-login-old", sock)
        run(sock, "login root", 2.0); show("4-login-root", sock)
        run(sock, "passwd reguser NewPass2", 1.2); show("5-passwd-new", sock)
        run(sock, "login reguser NewPass2", 2.0); show("6-login-new", sock)
        run(sock, "whoami", 1.0); show("7-whoami", sock)
        run(sock, "login root", 2.0); show("8-login-root2", sock)
        run(sock, "edit -c /etc/passwd", 1.5); show("9-passwd-file", sock)
    finally:
        kill_qemu(proc)
        print("== qemu log tail ==")
        try:
            print(open("/tmp/f11_qemu_debug3.log").read()[-1500:])
        except Exception as e:
            print("no log", e)


if __name__ == "__main__":
    main()
