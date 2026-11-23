#!/usr/bin/env python3
"""fs_coh_mount.py -- cache coherency scenario 4: mount / umount volume switch.

The one-slot path dirent cache and cluster snapshot must be invalidated when
the mount table changes: after `mount 2 /docs`, /docs/note.txt must resolve
against the secondary volume; after `umount /docs`, the same path must resolve
against the root volume (empty docs dir) and must not show stale P2 content.

Boots the dual-partition scratch image built by mkpart_disk.py (never the
repo reference disk).  Screen text is read through the VGA frame buffer after
switching to the text back-end, same as vprobe_regress.py.

usage:
  python tests/fs_coh_mount.py [catos.iso]
"""
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qemu_driver import (boot, clear_screen, find_qemu, force_text,
                          kill_qemu, screen_text, sendkey, type_text,
                          wait_for_prompt)  # noqa: E402

REPO = os.path.dirname(HERE)
ISO = os.path.join(REPO, "kernel_v25", "catos.iso")
DISK = os.path.join(HERE, "qemu_disk_part.img")
PORT = 4474
NOTE = "hello from partition 2"
RAW_LOG = os.path.join(REPO, "artifacts", "perf-fs",
                       "fs-coh-mount-screen-raw.txt")


class R(object):
    def __init__(self):
        self.fails = []

    def check(self, name, cond, detail=""):
        ok = bool(cond)
        print("  %-38s %s" % (name, "PASS" if ok else "FAIL"))
        if not ok:
            self.fails.append(name)
            if detail:
                print("      " + detail[-240:].replace("\n", " "))


RAWF = None

def run(s, c, wait=1.6):
    type_text(s, c)
    sendkey(s, "ret")
    time.sleep(wait)
    out = screen_text(s)
    if RAWF:
        RAWF.write("$ %s\n%s\n\n" % (c, out))
        RAWF.flush()
    return out


def main():
    iso = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith("--") else ISO
    qemu = find_qemu(None)
    if not os.path.exists(DISK):
        import subprocess
        subprocess.run([sys.executable, os.path.join(HERE, "mkpart_disk.py"),
                        "-o", DISK], check=True)

    proc, sock = boot(qemu, iso, PORT, disk=DISK)
    r = R()
    global RAWF
    RAWF = open(RAW_LOG, "w")
    try:
        force_text(sock)
        if not wait_for_prompt(sock, timeout=90.0):
            print("FATAL: shell prompt never appeared")
            return 2

        # Warm the root-volume path cache on purpose: two reads of a root
        # file so the rcache/cc slots are populated before the mount switch.
        run(sock, "edit -c /etc/passwd", 1.2)
        out = run(sock, "edit -c /etc/passwd", 1.2)
        r.check("root file readable pre-mount", "root:" in out, out)

        # Bind partition 2 onto /docs.
        out = run(sock, "mount 2 /docs", 2.0)
        r.check("mount 2 /docs succeeded", "mounted" in out, out)

        out = run(sock, "ls /docs", 1.4)
        r.check("note.txt visible on mounted volume", "note.txt" in out, out)

        out = run(sock, "edit -c /docs/note.txt", 1.6)
        r.check("mounted volume content, no stale root dirent",
                NOTE in out, out)

        # Detach: /docs goes back to the root-volume empty dir.
        out = run(sock, "umount /docs", 2.0)
        r.check("umount /docs detached", "detached" in out, out)

        clear_screen(sock, 1.0)
        out = run(sock, "ls /docs", 1.4)
        r.check("note.txt gone after umount", "note.txt" not in out, out)

        clear_screen(sock, 1.0)
        out = run(sock, "edit -c /docs/note.txt", 1.6)
        r.check("post-umount read misses stale P2 content", NOTE not in out, out)

        # Remount: slot reuse after detach must still resolve P2 fresh.
        out = run(sock, "mount 2 /docs", 2.0)
        r.check("remount after umount succeeds", "mounted" in out, out)
        out = run(sock, "edit -c /docs/note.txt", 1.6)
        r.check("remount reads P2 content again", NOTE in out, out)
        run(sock, "umount /docs", 2.0)

        print("mount coherency: %s" % ("PASS" if not r.fails else "FAIL: " + ",".join(r.fails)))
        return 0 if not r.fails else 1
    finally:
        kill_qemu(proc)


if __name__ == "__main__":
    sys.exit(main())
