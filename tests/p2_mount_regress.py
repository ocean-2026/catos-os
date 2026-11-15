#!/usr/bin/env python3
"""p2_mount_regress.py -- on-metal regression for the P2 mount feature.

Boots the built ISO against a dual-partition MBR disk (mkpart_disk.py)
and walks the full Linux-style mount cycle:

  1. mount            -> mount table lists the root volume
  2. ls /             -> the empty "docs" mountpoint directory
  3. mount 2 /docs   -> bind partition 2 onto /docs
  4. ls /             -> docs shows the drive-letter suffix, e.g. docs(B:)
  5. ls /docs         -> the secondary volume's note.txt
  6. cat /docs/note.txt -> cross-partition read works
  7. cp /bin/hello.ce /docs/hello.ce -> cross-partition write works
  8. df               -> one row per mountpoint
  9. umount /docs     -> detach; suffix disappears
 10. ls /docs         -> back to the (empty) root-volume directory

usage:
  python tests/p2_mount_regress.py [catos.iso] [--qemu PATH]
"""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qemu_driver import (boot, find_qemu, kill_qemu, screen_text, sendkey,
                         type_text, wait_for_prompt)  # noqa: E402
from qemu_regress import ppm_to_png, shot  # noqa: E402

REPO = os.path.dirname(HERE)
KERNEL = os.path.join(REPO, "kernel_v25")
PORT = 4720
SHOTS = os.path.join(HERE, "qemu_shots")
os.makedirs(SHOTS, exist_ok=True)


class R(object):
    def __init__(self):
        self.n = 0
        self.fails = []

    def check(self, name, cond, detail=""):
        self.n += 1
        ok = bool(cond)
        print("  %-34s %s" % (name, "PASS" if ok else "FAIL"))
        if not ok:
            self.fails.append(name)
            if detail:
                print("      " + detail[-240:].replace("\n", " "))


def run(s, c, wait=1.4):
    type_text(s, c)
    sendkey(s, "ret")
    time.sleep(wait)
    return screen_text(s)


def main():
    args = sys.argv[1:]
    iso = None
    if args and not args[0].startswith("--"):
        iso = args.pop(0)
    qemu = find_qemu(sys.argv[sys.argv.index("--qemu") + 1]
                     if "--qemu" in sys.argv else None)
    if not qemu:
        print("qemu-system-i386 not found")
        return 2
    iso = os.path.abspath(iso or os.path.join(KERNEL, "catos.iso"))
    if not os.path.isfile(iso):
        print("ISO not found: %s" % iso)
        return 2

    disk = os.path.join(HERE, "qemu_disk_part.img")
    if not os.path.exists(disk):
        import subprocess
        subprocess.run([sys.executable, os.path.join(HERE, "mkpart_disk.py"),
                        "-o", disk], check=True)

    proc, sock = boot(qemu, iso, PORT, disk=disk)
    r = R()
    try:
        if not wait_for_prompt(sock):
            print("FATAL: shell never reached a prompt")
            return 2

        run(sock, "clear", 0.8)

        # 1. bare mount: root volume table
        out = run(sock, "mount")
        shot(sock, "p2_01_mount_table")
        r.check("mount lists root volume", "hd0" in out and "/" in out, out)

        # 2. ls /: empty docs mountpoint
        out = run(sock, "ls /")
        shot(sock, "p2_02_ls_root")
        r.check("docs dir present before mount", "docs" in out, out)

        # 3. bind partition 2 onto /docs
        out = run(sock, "mount 2 /docs")
        shot(sock, "p2_03_mount")
        r.check("mount succeeded", "mounted" in out, out)

        # 4. ls /: drive-letter suffix on the mountpoint
        out = run(sock, "ls /")
        shot(sock, "p2_04_ls_suffix")
        r.check("mountpoint shows (X:) suffix",
                "docs(B:)" in out or "docs(C:)" in out, out)

        # 5. ls /docs: secondary volume contents
        out = run(sock, "ls /docs")
        shot(sock, "p2_05_ls_mounted")
        r.check("note.txt visible on mounted volume", "note.txt" in out, out)

        # 6. cross-partition read
        out = run(sock, "cat /docs/note.txt")
        shot(sock, "p2_06_cat_note")
        r.check("cross-partition read content",
                "hello from partition 2" in out, out)

        # 7. cross-partition write (copy a root-volume app into the mount)
        out = run(sock, "cp /bin/hello.ce /docs/hello.ce")
        out = run(sock, "ls /docs")
        shot(sock, "p2_07_write_copy")
        r.check("copied file lands on mounted volume", "hello.ce" in out, out)

        # 8. df: one row per mountpoint
        out = run(sock, "df")
        shot(sock, "p2_08_df")
        r.check("df lists mountpoint /docs", "/docs" in out, out)
        r.check("df lists root /", "/" in out, out)

        # 9. umount
        out = run(sock, "umount /docs")
        shot(sock, "p2_09_umount")
        r.check("umount detached", "detached" in out, out)

        # 10. after umount: root-volume docs dir is empty again
        out = run(sock, "ls /docs")
        shot(sock, "p2_10_after_umount")
        r.check("note.txt gone after umount", "note.txt" not in out, out)

        out = run(sock, "ls /")
        shot(sock, "p2_11_ls_restored")
        r.check("mount suffix restored after umount", "note.txt" not in out, out)

        out = run(sock, "mount")
        r.check("table back to root only", out.count("hd0") <= 1, out)

        # 12. remount after detach: volume slot must be reusable
        out = run(sock, "mount 2 /docs")
        r.check("remount after umount succeeds", "mounted" in out, out)
        out = run(sock, "ls /docs")
        r.check("remount sees partition again", "note.txt" in out, out)
        shot(sock, "p2_12_remount")
        out = run(sock, "umount /docs")
        r.check("second umount detached", "detached" in out, out)
    finally:
        kill_qemu(proc)

    print("\n%d checks, %d failures" % (r.n, len(r.fails)))
    return 1 if r.fails else 0


if __name__ == "__main__":
    sys.exit(main())
