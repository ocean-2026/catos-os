#!/usr/bin/env python3
import os, sys, time, socket, subprocess, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from f11_lib import ISO, stage_disk, find_qemu, boot_guest, settle, kill_qemu, type_text, sendkey, read_screen

PORT = 4491

def raw(sock, c, wait=1.0):
    sock.sendall((c + "\n").encode())
    time.sleep(wait)
    sock.setblocking(False)
    chunks = []
    time.sleep(0.2)
    try:
        while True:
            b = sock.recv(65536)
            if not b: break
            chunks.append(b)
    except BlockingIOError:
        pass
    finally:
        sock.setblocking(True)
    return b"".join(chunks).decode("latin-1", "replace")

def main():
    disk = stage_disk("diag")
    proc, sock = boot_guest(ISO, disk, PORT)
    try:
        settle(sock)
        print("== info status ==")
        print(raw(sock, "info status", 1.0))
        print("== xp 64 words ==")
        r = raw(sock, "xp /64 0xb8000", 1.5)
        print(repr(r[:600]))
        type_text(sock, "whoami"); sendkey(sock, "ret")
        time.sleep(1.5)
        print("== after whoami, screen text ==")
        print(read_screen(sock))
        print("== xp again ==")
        r = raw(sock, "xp /64 0xb8000", 1.5)
        print(repr(r[:600]))
    finally:
        kill_qemu(proc)

if __name__ == "__main__":
    main()
