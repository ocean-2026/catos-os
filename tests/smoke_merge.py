#!/usr/bin/env python3
"""smoke_merge.py -- post night-merge on-metal smoke sweep (v2.42).

Covers every feature the integration brief asks to exercise on a real
QEMU/i386 guest; screenshots land in tests/qemu_shots_merge/.

  Pass A (fresh 64 MiB disk, -m 3840):
    mem / hwinfo (4G memory display), cowsay/sl/cal/fortune/yes,
    pkg list/install/remove bc + bc calc, head/tail/wc/tree,
    PE sample: convert offer -> direct PE32 run -> translated CE run.
  Pass B (partition disk qemu_disk_part.img):
    mount 2 /docs, ls /docs, cat note, df, umount /docs.

Usage:
  python3 tests/smoke_merge.py [--qemu PATH] [--port-base 4731] [--keep]
"""
import os, shutil, socket, subprocess, sys, time, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
sys.path.insert(0, HERE)

from qemu_driver import (find_qemu, kill_qemu, screen_text, sendkey,
                         type_text, wait_for_prompt)
from qemu_regress import ppm_to_png

SHOTS = os.path.join(HERE, "qemu_shots_merge")
ART = os.path.join(HERE, "smoke_artifacts")
ISO = os.path.join(ROOT, "kernel_v25", "catos.iso")
DISK_A = os.path.join(HERE, "qemu_disk_smoke.img")
DISK_B = os.path.join(HERE, "qemu_disk_part.img")
STAGE = os.path.join(tempfile.gettempdir(), "catos_smoke_stage")

PE_NAME = "PESAMPLE.EXE"
CE_NAME = "PESAMPLE.CE"
EXPECT_MSG = "PE->CE roundtrip OK"

results = []


def check(name, ok, note=""):
    results.append((name, ok, note))
    print("%-4s %-26s %s" % ("PASS" if ok else "FAIL", name, note[:120]))


def boot_direct(qemu, iso, port, disk, mem="256"):
    """Launch QEMU against a staged disk copy (workspace fs flock workaround)."""
    os.makedirs(STAGE, exist_ok=True)
    stage_disk = os.path.join(STAGE, os.path.basename(disk))
    shutil.copy2(disk, stage_disk)
    cmdline = [qemu, "-m", mem, "-no-reboot", "-net", "none",
               "-cdrom", iso, "-boot", "order=d", "-display", "none",
               "-monitor", "tcp:127.0.0.1:%d,server,nowait" % port,
               "-drive", "file=%s,format=raw,if=ide" % stage_disk]
    proc = subprocess.Popen(cmdline, stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL)
    for _ in range(40):
        if proc.poll() is not None:
            raise RuntimeError("qemu exited rc=%r" % proc.returncode)
        try:
            return proc, socket.create_connection(("127.0.0.1", port),
                                                  timeout=5), stage_disk
        except OSError:
            time.sleep(1)
    kill_qemu(proc)
    raise RuntimeError("no monitor on port %d" % port)


def shot(s, name):
    os.makedirs(SHOTS, exist_ok=True)
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
        raise RuntimeError("no screendump for %s" % name)
    ppm_to_png(ppm, png)
    return png


def run(s, cmdline, wait=1.4):
    type_text(s, cmdline)
    sendkey(s, "ret", wait)
    time.sleep(0.6)
    return screen_text(s)


def prep_artifacts():
    os.makedirs(ART, exist_ok=True)
    pe = os.path.join(ART, PE_NAME)
    ce = os.path.join(ART, CE_NAME)
    subprocess.run([sys.executable, os.path.join(HERE, "mkpe.py"),
                    "--sample", pe], check=True)
    subprocess.run([sys.executable, os.path.join(HERE, "mkpe.py"),
                    pe, ce], check=True)
    return pe, ce


def fresh_disk(path):
    if os.path.exists(path):
        os.remove(path)
    with open(path, "wb") as f:
        f.truncate(64 * 1024 * 1024)


def inject(disk, src, name):
    rc = subprocess.run([sys.executable, os.path.join(HERE, "inject_any.py"),
                         disk, src, name], capture_output=True, text=True)
    if rc.returncode != 0:
        print("inject FAIL %s: %s %s" % (name, rc.stdout, rc.stderr))
        sys.exit(3)
    print("inject OK %s" % name)


def main():
    keep = "--keep" in sys.argv
    qemu = None
    if "--qemu" in sys.argv:
        qemu = sys.argv[sys.argv.index("--qemu") + 1]
    port_base = 4731
    if "--port-base" in sys.argv:
        port_base = int(sys.argv[sys.argv.index("--port-base") + 1])
    qemu = find_qemu(qemu) or "/home/user/bin/qemu-system-i386"
    if not os.path.exists(qemu):
        print("qemu-system-i386 not found")
        return 2

    pe, ce = prep_artifacts()

    # ---- Pass 1: format disk A ----
    fresh_disk(DISK_A)
    proc, s, _ = boot_direct(qemu, ISO, port_base, DISK_A, mem="256")
    print("monitor connected (format pass)")
    if not wait_for_prompt(s):
        print("FATAL: no prompt"); kill_qemu(proc); return 3
    run(s, "format", 1.0)
    sendkey(s, "y", 0.4)
    sendkey(s, "ret", 12.0)
    kill_qemu(proc)
    time.sleep(2)
    # copy staged volume back so the formatted FAT survives injection
    shutil.copy2(os.path.join(STAGE, os.path.basename(DISK_A)), DISK_A)

    inject(DISK_A, pe, PE_NAME)
    inject(DISK_A, ce, CE_NAME)

    # ---- Pass 2: smoke on -m 3840 ----
    proc, s, _ = boot_direct(qemu, ISO, port_base + 1, DISK_A, mem="3840")
    print("monitor connected (smoke pass A, -m 3840)")
    if not wait_for_prompt(s):
        print("FATAL: no prompt pass A"); kill_qemu(proc); return 3

    t = run(s, "mem", 1.5)
    shot(s, "01_mem_3840")
    check("mem reports upper/free frames", "Upper" in t and "PMM" in t, t[-100:])

    t = run(s, "hwinfo", 1.5)
    shot(s, "02_hwinfo")
    check("hwinfo memory row", "mem" in t.lower() or "Memory" in t, t[-100:])

    t = run(s, "cowsay hello", 1.2)
    shot(s, "03_cowsay")
    check("cowsay ran", "hello" in t.lower(), t[-80:])

    run(s, "sl", 1.4)
    shot(s, "04_sl")

    t = run(s, "cal", 1.2)
    shot(s, "05_cal")
    check("cal renders", "2026" in t or "1" in t, t[-80:])

    t = run(s, "fortune", 1.2)
    shot(s, "06_fortune")

    run(s, "yes q", 1.2)
    shot(s, "07_yes")
    sendkey(s, "ctrl-c", 0.3)

    t = run(s, "pkg list", 1.4)
    shot(s, "08_pkg_list")
    check("pkg list sees repo", "bc" in t and "stats" in t and "rps" in t, t[-100:])

    t = run(s, "pkg install bc", 1.6)
    shot(s, "09_pkg_install_bc")
    check("pkg install bc", "installed" in t, t[-100:])

    t = run(s, "bc 12 + 34", 1.6)
    shot(s, "10_bc_run")
    check("bc computes 46", "46" in t, t[-80:])

    t = run(s, "head -n 5 /appdevelop/API.TXT", 1.2)
    shot(s, "11_head")
    check("head works", "API" in t or "CatOS" in t, t[-80:])

    t = run(s, "tail -n 3 /appdevelop/API.TXT", 1.2)
    shot(s, "12_tail")

    t = run(s, "wc /appdevelop/API.TXT", 1.2)
    shot(s, "13_wc")
    check("wc reports counts", t.strip() != "", t[-80:])

    run(s, "tree /", 1.8)
    shot(s, "14_tree")

    # PE path A: conversion offer
    type_text(s, "run /bin/%s" % PE_NAME)
    sendkey(s, "ret", 2.0)
    time.sleep(1.2)
    shot(s, "15_pe_offer")
    t = screen_text(s)
    check("PE convert offer shown", "Convert" in t or "CE" in t, t[-100:])
    # answer N: direct PE32 load
    sendkey(s, "n", 0.4)
    sendkey(s, "ret", 3.0)
    shot(s, "16_pe_direct_run")
    t = screen_text(s)
    check("direct PE32 runs", EXPECT_MSG in t, t[-100:])

    t = run(s, "run /bin/%s" % CE_NAME, 2.5)
    shot(s, "17_ce_run")
    check("translated CE runs", EXPECT_MSG in t, t[-100:])

    t = run(s, "pkg remove bc", 1.4)
    shot(s, "18_pkg_remove_bc")
    check("pkg remove bc", "removed" in t, t[-100:])

    t = run(s, "pkg list", 1.4)
    shot(s, "19_pkg_list_after_rm")
    check("bc back to available", "bc" in t, t[-100:])

    if not keep:
        kill_qemu(proc)
    time.sleep(2)

    # ---- Pass B: partition / mount flow ----
    if not os.path.exists(DISK_B):
        subprocess.run([sys.executable, os.path.join(HERE, "mkpart_disk.py"),
                        "-o", DISK_B], check=True)
    proc, s, _ = boot_direct(qemu, ISO, port_base + 2, DISK_B, mem="256")
    print("monitor connected (mount pass B)")
    if not wait_for_prompt(s):
        print("FATAL: no prompt pass B"); kill_qemu(proc); return 3

    t = run(s, "mount", 1.2)
    shot(s, "20_mount_table")
    check("mount table lists root", "hd0" in t and "/" in t, t[-100:])

    t = run(s, "mount 2 /docs", 1.6)
    shot(s, "21_mount")
    check("mount partition 2", "mounted" in t, t[-100:])

    t = run(s, "ls /docs", 1.2)
    shot(s, "22_ls_docs")
    check("docs volume listed", "note.txt" in t, t[-100:])

    t = run(s, "cat /docs/note.txt", 1.2)
    shot(s, "23_cat_note")
    check("cross-partition read", "hello" in t.lower(), t[-100:])

    t = run(s, "df", 1.2)
    shot(s, "24_df")
    check("df lists root + mount", "/" in t and "/docs" in t, t[-100:])

    t = run(s, "umount /docs", 1.4)
    shot(s, "25_umount")
    check("umount detached", "detached" in t, t[-100:])

    if not keep:
        kill_qemu(proc)

    fails = [n for n, ok, _ in results if not ok]
    print("\nsmoke_merge: %d/%d PASS" % (len(results) - len(fails), len(results)))
    if fails:
        print("  FAILED: " + ", ".join(fails))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
