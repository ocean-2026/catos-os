#!/usr/bin/env python3
"""p4_pkg_test.py - P4 package-manager + migrated-tools end-to-end driver.

Boots catos.iso with a blank IDE disk, formats it, then exercises:
  pkg list / info / install / remove  (bc, stats, rps packages)
  bc / stats / rps installed-package programs
  head / tail / wc / tree migrated shell tools

Screendumps land in tests/qemu_shots_p4/*.png.  Text-level verification
is done by a human reading the PNGs (no OCR dependency), matching the
qemu_regress.py convention.

NIGHT-P4 note: the default qemu_driver._stage_image copies images to the
shared /tmp/catos_qemu_stage directory, which concurrent parallel agents
overwrite.  This driver stages into a PRIVATE per-agent dir instead, so our
guest never boots another agent's stale image.

usage:
  python3 tests/p4_pkg_test.py [catos.iso] [--port 4740] [--qemu PATH]
"""
import argparse
import glob
import os
import shutil
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from qemu_driver import sendkey, type_text, kill_qemu
from qemu_regress import ppm_to_png, find_qemu

SHOTS = os.path.join(HERE, "qemu_shots_p4")
# private stage dir under /tmp (flock-capable, unlike the hpvs_fs workspace);
# NOT the shared /tmp/catos_qemu_stage that concurrent agents overwrite.
STAGE = "/tmp/catos_p4_stage"


def stage_private(src):
    """Copy iso/disk into a private per-agent stage dir."""
    os.makedirs(STAGE, exist_ok=True)
    dst = os.path.join(STAGE, os.path.basename(src))
    shutil.copy2(src, dst)
    return dst


def boot_private(qemu, iso, port, disk=None):
    iso = stage_private(iso)
    if disk:
        disk = stage_private(disk)
    cmdline = [qemu, "-m", "256", "-no-reboot", "-net", "none",
               "-cdrom", iso, "-boot", "order=d", "-display", "none",
               "-monitor", "tcp:127.0.0.1:%d,server,nowait" % port]
    if disk:
        cmdline += ["-drive", "file=%s,format=raw,if=ide" % disk]
    proc = subprocess.Popen(cmdline, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    for _ in range(40):
        if proc.poll() is not None:
            raise RuntimeError("qemu exited rc=%r" % proc.returncode)
        try:
            return proc, socket.create_connection(("127.0.0.1", port),
                                                  timeout=5)
        except OSError:
            time.sleep(1)
    raise RuntimeError("could not reach the QEMU monitor")


def shot(s, name):
    png = os.path.join(SHOTS, name + ".png")
    ppm = os.path.join(SHOTS, name + ".ppm")
    if os.path.exists(ppm):
        os.remove(ppm)
    s.sendall(("screendump %s\n" % ppm.replace("\\", "/")).encode())
    time.sleep(0.5)
    for _ in range(40):
        if os.path.exists(ppm) and os.path.getsize(ppm) > 0:
            break
        time.sleep(0.5)
    else:
        raise RuntimeError("screendump produced no file for %s" % name)
    ppm_to_png(ppm, png)
    return png


def do(s, cmdline, wait=1.4):
    type_text(s, cmdline)
    sendkey(s, "ret", wait)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("iso", nargs="?")
    ap.add_argument("--port", type=int, default=4740)
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    root = os.path.dirname(HERE)
    iso = args.iso or os.path.join(root, "kernel_v25", "catos.iso")
    qemu = find_qemu(args.qemu)
    if not qemu:
        print("qemu-system-i386 not found")
        return 2
    os.makedirs(SHOTS, exist_ok=True)
    for old in glob.glob(os.path.join(SHOTS, "*.png")):
        os.remove(old)

    disk = os.path.join(HERE, "qemu_disk_p4.img")
    if not os.path.exists(disk):
        with open(disk, "wb") as f:
            f.truncate(64 * 1024 * 1024)

    proc, s = boot_private(qemu, iso, args.port, disk=disk)
    print("monitor connected on port %d" % args.port)
    time.sleep(25)                       # TCG slow: GRUB + boot banner

    # blank disk -> format FAT32 (consent: physical 'y' + Enter)
    do(s, "format", 1.0)
    sendkey(s, "y", 0.4)
    sendkey(s, "ret", 8.0)

    steps = [
        ("p4_01_pkg_list",        "pkg list"),
        ("p4_02_pkg_info_bc",    "pkg info bc"),
        ("p4_03_install_bc",     "pkg install bc"),
        ("p4_04_bc_run",         "bc 12 + 34"),
        ("p4_05_install_stats",   "pkg install stats"),
        ("p4_06_make_sample",     "hello > /tmp/sample.txt"),
        ("p4_07_stats_run",      "stats /tmp/sample.txt"),
        ("p4_08_install_rps",     "pkg install rps"),
        ("p4_09_rps_run",         "rps paper"),
        ("p4_10_wc",             "wc /tmp/sample.txt"),
        ("p4_11_head",           "head -n 5 /appdevelop/API.TXT"),
        ("p4_12_tail",           "tail -n 3 /appdevelop/API.TXT"),
        ("p4_13_tree",           "tree /"),
        ("p4_14_remove_bc",      "pkg remove bc"),
        ("p4_15_list_after_rm",  "pkg list"),
    ]

    for name, cmd in steps:
        print("step %-22s %s" % (name, cmd))
        do(s, cmd, 1.6)
        shot(s, name)

    if not args.keep:
        kill_qemu(proc)
    print("shots in %s" % SHOTS)
    return 0


if __name__ == "__main__":
    sys.exit(main())
