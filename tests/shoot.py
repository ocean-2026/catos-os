#!/usr/bin/env python3
"""shoot.py -- boot CatOS, drive a command list, save PPM screenshots.

Usage:
    python3 shoot.py --iso path.iso --out shots --cmd "hive" --cmd "theme light"
Screenshots land as shots/NN-<slug>.ppm (convert with ImageMagick).
"""
import argparse, os, sys, time, re, subprocess
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_driver import boot, kill_qemu, cmd, sendkey, type_text, read_screen, screen_text, force_text, wait_for_prompt


def shot(s, sock, out, n, slug):
    p = os.path.join(out, "%02d-%s.ppm" % (n, slug))
    cmd(s, "screendump %s" % p, wait=1.5)
    for _ in range(30):
        if os.path.exists(p):
            break
        time.sleep(0.3)
    print("shot %s (%s bytes)" % (p, os.path.getsize(p) if os.path.exists(p) else "MISSING"))


def run_line(s, line, wait=1.2):
    type_text(s, line)
    sendkey(s, "ret", wait)
    time.sleep(wait)
    # the pager may park on "More": page through
    for _ in range(12):
        t = screen_text(s)
        if "more" not in t.lower():
            break
        sendkey(s, "spc", 0.4)
    return read_screen(s)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iso", required=True)
    ap.add_argument("--disk")
    ap.add_argument("--out", required=True)
    ap.add_argument("--cmd", action="append", default=[])
    ap.add_argument("--port", type=int, default=4455)
    ap.add_argument("--textmode", action="store_true", default=True)
    ap.add_argument("--settle", type=float, default=22.0)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)

    qemu = "qemu-system-i386"
    proc, sock = boot(qemu, args.iso, args.port, disk=args.disk)
    try:
        time.sleep(args.settle)
        wait_for_prompt(sock, timeout=60)
        force_text(sock)
        wait_for_prompt(sock, timeout=30)
        n = 0
        shot(sock, sock, args.out, n, "prompt"); n += 1
        for i, line in enumerate(args.cmd, 1):
            out = run_line(sock, line)
            print("--- %s ---" % line)
            print(out)
            shot(sock, sock, args.out, n, "cmd%02d" % i); n += 1
        print("guest alive:", proc.poll() is None)
    finally:
        kill_qemu(proc)


if __name__ == "__main__":
    main()
