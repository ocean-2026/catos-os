#!/usr/bin/env python3
"""fs_coh_format.py -- cache coherency case 7 (format wipe).

After a guest-side FAT reformat of the *private staged* volume, the one-slot
path dirent cache must not hand back a stale entry for a file that existed
before the wipe.  Boots the bench ISO against a throwaway copy of the
reference disk (the repo image catos_disk.img is never mounted writable),
types `bench fsformat`, and parses the `[perf] coh case=7` serial line.

usage:
  python tests/fs_coh_format.py [catos.iso]
"""
import os
import re
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qemu_driver import (boot, find_qemu, force_text, kill_qemu,  # noqa: E402
                         sendkey, type_text, wait_for_prompt)

REPO = os.path.dirname(HERE)
ISO = os.path.join(REPO, "kernel_v25", "catos.iso")
DISK_SRC = "/home/user/work/catos-os/kernel_v25/catos_disk.img"
PORT = 4473
LOG = os.path.join(tempfile.gettempdir(), "fs_coh_format_serial.log")
CRASH = ("page fault", "kernel fault", "system halted")


def main():
    iso = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith("--") else ISO
    qemu = find_qemu(None)
    if os.path.exists(LOG):
        os.remove(LOG)

    proc, sock = boot(qemu, iso, PORT, disk=DISK_SRC,
                      extra_args=["-serial", "file:" + LOG], monitor_wait=60)
    try:
        force_text(sock)
        if not wait_for_prompt(sock, timeout=90.0):
            print("FATAL: shell prompt never appeared")
            return 3

        type_text(sock, "bench fsformat")
        sendkey(sock, "ret", 3.0)

        deadline = time.time() + 60.0
        line = None
        while time.time() < deadline:
            if os.path.exists(LOG):
                txt = open(LOG, errors="replace").read()
                low = txt.lower()
                if any(m in low for m in CRASH):
                    print("FAIL: kernel fault after wipe")
                    print(txt[-300:])
                    return 4
                m = re.search(r"\[perf\] coh case=7[^\n]*", txt)
                if m:
                    line = m.group(0)
                    break
            time.sleep(0.5)

        if not line:
            print("FAIL: timeout waiting for coh case=7 verdict")
            return 5

        print(line)
        mm = re.search(r"pass=(\d+)", line)
        ok = bool(mm and mm.group(1) == "1")
        print("case 7 (post-wipe stale dirent):", "PASS" if ok else "FAIL")
        return 0 if ok else 1
    finally:
        kill_qemu(proc)


if __name__ == "__main__":
    sys.exit(main())
