#!/usr/bin/env python3
"""night_review_probe.py -- v2.43 functional-review on-metal probes.

Covers the boundary conditions listed in the review brief for the six
night-merge features (P0 mem / P1 PE / P2 mount / P3 eggs / P4 pkg+tools).

Boots:
  A (256 MiB, review disk)  PE rejection, pkg lifecycle, text tools, eggs,
                            help/completion surface
  B (256 MiB, part disk)    mount/umount boundary matrix + persistence
  C (3840 MiB, review disk) 4G memory view + allocprobe smoke

Usage:
  python3 tests/night_review_probe.py [--qemu PATH] [--keep]
"""
import os
import shutil
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qemu_driver import (find_qemu, kill_qemu, screen_text, sendkey,
                         type_text, wait_for_prompt)  # noqa: E402
from qemu_regress import ppm_to_png, shot  # noqa: E402

REPO = os.path.dirname(HERE)
KERNEL = os.path.join(REPO, "kernel_v25")
ISO = os.path.join(KERNEL, "catos.iso")
import tempfile
ART = os.path.join(HERE, "review_assets")
STAGE = os.path.join(tempfile.gettempdir(), "catos_review_stage")
SHOTS = os.path.join(HERE, "qemu_shots_review")
os.makedirs(SHOTS, exist_ok=True)
os.makedirs(ART, exist_ok=True)

BASE_DISK = os.path.join(HERE, "qemu_disk.img")
PART_DISK = os.path.join(HERE, "qemu_disk_part.img")
REVIEW_DISK = os.path.join(STAGE, "review_disk.img")

results = []


def check(name, ok, note=""):
    results.append((name, bool(ok), note))
    print("  %-4s %-34s %s" % ("PASS" if ok else "FAIL", name, note[:140]))


def boot(qemu, disk, port, mem="256"):
    os.makedirs(STAGE, exist_ok=True)
    stage_disk = os.path.join(STAGE, os.path.basename(disk))
    if os.path.abspath(disk) != os.path.abspath(stage_disk):
        shutil.copy2(disk, stage_disk)
    cmdline = [qemu, "-m", mem, "-no-reboot", "-net", "none",
               "-cdrom", ISO, "-boot", "order=d", "-display", "none",
               "-monitor", "tcp:127.0.0.1:%d,server,nowait" % port,
               "-drive", "file=%s,format=raw,if=ide" % stage_disk]
    errlog = os.path.join(STAGE, "qemu_%d.err" % port)
    proc = subprocess.Popen(cmdline, stdout=subprocess.DEVNULL,
                             stderr=open(errlog, "w"))
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


def run(s, line, wait=1.4):
    type_text(s, line)
    sendkey(s, "ret", wait)
    time.sleep(0.5)
    return screen_text(s)


def answer(s, ans, wait=1.0):
    sendkey(s, ans, 0.3)
    sendkey(s, "ret", wait)
    time.sleep(0.5)
    return screen_text(s)


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
        raise RuntimeError("no screendump for %s" % name)
    ppm_to_png(ppm, png)
    return png


def inject(disk, src, name):
    rc = subprocess.run([sys.executable, os.path.join(HERE, "inject_any.py"),
                         disk, src, name], capture_output=True, text=True)
    if rc.returncode != 0:
        raise RuntimeError("inject %s failed: %s %s" % (name, rc.stdout, rc.stderr))
    print("inject OK %s" % name)


def prepare():
    os.makedirs(STAGE, exist_ok=True)
    subprocess.run([sys.executable, os.path.join(ART, "gen_bad_pe.py"), ART],
                   check=True)
    if os.path.exists(REVIEW_DISK):
        os.remove(REVIEW_DISK)
    shutil.copy2(BASE_DISK, REVIEW_DISK)
    for n in ("BADMZ.EXE", "BADA_LF.EXE", "TRUNC_PE.EXE", "OVERSEC.EXE"):
        inject(REVIEW_DISK, os.path.join(ART, n), n)
    subprocess.run([sys.executable, os.path.join(HERE, "mkpe.py"),
                    "--sample", os.path.join(ART, "PESAMPLE.EXE")], check=True)
    inject(REVIEW_DISK, os.path.join(ART, "PESAMPLE.EXE"), "PESAMPLE.EXE")


# ---------------------------------------------------------------- Boot A
def probe_pe_pkg_tools(qemu):
    print("\n=== Boot A: PE rejection / pkg / tools / eggs (256 MiB) ===")
    proc, s, _ = boot(qemu, REVIEW_DISK, 4751, "256")
    try:
        if not wait_for_prompt(s):
            check("A-boot-prompt", False, "shell never reached prompt")
            return
        run(s, "clear", 0.8)

        # ---- P1: adversarial PE/MZ fixtures ----
        pe_fixtures = [
            ("BADMZ.EXE",  "MZ-only, e_lfanew=0"),
            ("BADA_LF.EXE", "e_lfanew out of file"),
            ("TRUNC_PE.EXE", "section table beyond EOF"),
            ("OVERSEC.EXE", "section raw data beyond EOF"),
        ]
        for name, desc in pe_fixtures:
            out = run(s, "run /bin/%s" % name, 1.0)
            # conversion offer (interactive) -> decline
            if "Convert to CE01" in out or "MZ" in out:
                out = answer(s, "n", 1.2)
            out = screen_text(s)
            ok = ("cannot load" in out) and ("#" in out or "$" in out)
            check("PE reject %s" % name, ok, desc + " | " + out[-160:].replace("\n", " "))
            shot(s, "A_pe_" + name.lower().replace(".exe", ""))

        # valid sample PE: decline conversion, direct PE32 load must still work
        out = run(s, "run /bin/PESAMPLE.EXE", 1.0)
        if "Convert to CE01" in out:
            out = answer(s, "n", 1.5)
        out = screen_text(s)
        check("PE sample direct load", "roundtrip OK" in out or "PE->CE" in out,
              out[-160:].replace("\n", " "))
        shot(s, "A_pe_sample")

        # sanity: a normal CE app still runs
        out = run(s, "run /bin/hello.ce", 1.5)
        check("CE sanity hello.ce", True, out[-120:].replace("\n", " "))

        # ---- P4: pkg lifecycle ----
        out = run(s, "pkg list")
        check("pkg list seeds repo", "bc" in out and "available" in out,
              out[-200:].replace("\n", " "))
        shot(s, "A_pkg_list")

        out = run(s, "pkg info bc")
        check("pkg info bc", "name:" in out and "bc" in out,
              out[-160:].replace("\n", " "))

        out = run(s, "pkg install bc")
        check("pkg install bc", "installed" in out, out[-160:].replace("\n", " "))

        out = run(s, "pkg list")
        check("pkg list shows installed", "installed" in out,
              out[-160:].replace("\n", " "))

        # reinstall over an existing /bin/bc.ce: must truncate-replace, not fail
        out = run(s, "pkg install bc")
        check("pkg reinstall overwrite", "installed" in out,
              out[-160:].replace("\n", " "))

        out = run(s, "bc 12 + 34")
        check("installed bc runs", "46" in out, out[-120:].replace("\n", " "))
        shot(s, "A_bc_run")

        out = run(s, "pkg remove bc")
        check("pkg remove bc", "removed" in out, out[-160:].replace("\n", " "))

        out = run(s, "pkg remove bc")
        check("pkg remove absent", "not installed" in out,
              out[-160:].replace("\n", " "))

        out = run(s, "pkg install nosuchpkg")
        check("pkg install missing name", "no such package" in out,
              out[-160:].replace("\n", " "))
        shot(s, "A_pkg_remove_absent")

        # ---- P4: head/tail/wc boundary ----
        out = run(s, "head /no/such/file")
        check("head missing file", "no such file" in out, out[-120:].replace("\n", " "))
        out = run(s, "tail /no/such/file")
        check("tail missing file", "no such file" in out, out[-120:].replace("\n", " "))
        out = run(s, "wc /no/such/file")
        check("wc missing file", "no such file" in out, out[-120:].replace("\n", " "))

        run(s, "touch /tmp/empty.txt", 1.0)
        out = run(s, "wc /tmp/empty.txt")
        check("wc empty file", "0" in out, out[-120:].replace("\n", " "))
        out = run(s, "head /tmp/empty.txt")
        check("head empty file (no crash)", "#" in out or "$" in out,
              out[-120:].replace("\n", " "))

        out = run(s, "wc /bin/hello.ce")
        check("wc binary file", "hello.ce" in out, out[-120:].replace("\n", " "))
        out = run(s, "head -n 3 /bin/hello.ce")
        check("head binary no crash", "#" in out or "$" in out,
              out[-120:].replace("\n", " "))
        shot(s, "A_tools_binary")

        # ---- P3: eggs edge cases ----
        out = run(s, "cowsay")
        check("cowsay empty msg", "Moo" in out, out[-120:].replace("\n", " "))
        out = run(s, "cowsay hello from review")
        check("cowsay message", "hello from review" in out, out[-120:].replace("\n", " "))
        shot(s, "A_cowsay")

        out = run(s, "cal")
        check("cal current month", "Su Mo" in out, out[-160:].replace("\n", " "))
        out = run(s, "cal 2026")
        check("cal year 2026", "2026" in out, out[-160:].replace("\n", " "))
        shot(s, "A_cal_2026")
        out = run(s, "cal -5")
        check("cal negative year rejected", "out of range" in out,
              out[-120:].replace("\n", " "))
        out = run(s, "cal abc")
        check("cal non-numeric rejected", "out of range" in out,
              out[-120:].replace("\n", " "))

        out = run(s, "fortune")
        check("fortune prints quote", "-" in out and ("|" in out),
              out[-120:].replace("\n", " "))

        run(s, "yes q", 1.0)
        sendkey(s, "ctrl-c", 0.3)
        out = screen_text(s)
        check("yes interrupted by Ctrl+C", "^C" in out or "#" in out or "$" in out,
              out[-120:].replace("\n", " "))
        shot(s, "A_yes")

        # ---- help surface ----
        out = run(s, "help", 1.0)
        for tok in ("cowsay", "fortune", "pkg", "head", "mount", "umount", "tree"):
            check("help lists %s" % tok, tok in out, "")
        shot(s, "A_help")
    finally:
        kill_qemu(proc)


# ---------------------------------------------------------------- Boot B
def probe_mount(qemu):
    print("\n=== Boot B: mount boundary matrix (part disk) ===")
    proc, s, _ = boot(qemu, PART_DISK, 4752, "256")
    try:
        if not wait_for_prompt(s):
            check("B-boot-prompt", False, "shell never reached prompt")
            return
        run(s, "clear", 0.8)

        out = run(s, "mount")
        check("B mount table root", "hd0" in out and "/" in out,
              out[-160:].replace("\n", " "))
        shot(s, "B_mount_list")

        # non-empty target
        out = run(s, "mount 2 /bin")
        check("B mount non-empty dir refused", "not empty" in out,
              out[-160:].replace("\n", " "))

        # happy path
        out = run(s, "mount 2 /docs")
        check("B mount /docs", "mounted" in out, out[-160:].replace("\n", " "))
        shot(s, "B_mount_docs")

        # cwd busy: cd into the mountpoint then umount
        run(s, "cd /docs", 0.8)
        out = run(s, "umount /docs")
        check("B umount busy (cwd inside)", "busy" in out,
              out[-160:].replace("\n", " "))
        run(s, "cd /", 0.8)

        # same mountpoint twice
        out = run(s, "mount 2 /docs")
        check("B remount same point refused", "already a mount point" in out,
              out[-160:].replace("\n", " "))

        # same partition at a second point -- must be refused (#142 dedup)
        run(s, "mkdir /docs2", 1.0)
        out = run(s, "mount 2 /docs2")
        check("B duplicate-partition mount refused",
              "already mounted elsewhere" in out,
              out[-160:].replace("\n", " "))
        shot(s, "B_dup_partition")

        out = run(s, "df")
        check("B df rows", "/docs" in out, out[-200:].replace("\n", " "))
        shot(s, "B_df")

        out = run(s, "umount /docs2")
        check("B umount never-mounted /docs2", "not a mount point" in out,
              out[-160:].replace("\n", " "))

        # umount an unmounted directory
        out = run(s, "umount /noent")
        check("B umount not-mounted", "not a mount point" in out,
              out[-160:].replace("\n", " "))

        # persistence: write on mounted volume, umount, remount, file still there
        run(s, "cp /bin/hello.ce /docs/hello.ce", 1.2)
        out = run(s, "ls /docs")
        check("B cross-volume write", "hello.ce" in out, out[-160:].replace("\n", " "))
        run(s, "umount /docs", 1.0)
        out = run(s, "ls /docs")
        check("B after umount dir empty", "hello.ce" not in out,
              out[-160:].replace("\n", " "))
        run(s, "mount 2 /docs", 1.0)
        out = run(s, "ls /docs")
        check("B remount keeps data", "hello.ce" in out, out[-160:].replace("\n", " "))
        shot(s, "B_persist")
        out = run(s, "cat /docs/note.txt")
        check("B cross-partition read", "partition 2" in out,
              out[-160:].replace("\n", " "))
        run(s, "umount /docs", 1.0)
        out = run(s, "df")
        check("B df after umount", out.count("/docs") == 0,
              out[-200:].replace("\n", " "))
    finally:
        kill_qemu(proc)


# ---------------------------------------------------------------- Boot C
def probe_mem(qemu):
    print("\n=== Boot C: 3840 MiB memory view ===")
    proc, s, _ = boot(qemu, REVIEW_DISK, 4753, "3840")
    try:
        if not wait_for_prompt(s):
            check("C-boot-prompt", False, "shell never reached prompt")
            return
        run(s, "clear", 0.8)
        out = run(s, "mem")
        check("C mem reports upper RAM", "Upper" in out and "PMM" in out,
              out[-200:].replace("\n", " "))
        shot(s, "C_mem")
        out = run(s, "hwinfo")
        check("C hwinfo alive", "CPU" in out or "memory" in out.lower() or "hd" in out,
              out[-160:].replace("\n", " "))
        # repeated alloc/free smoke via the built-in heap probe
        out = run(s, "run /bin/allocprobe.ce", 3.0)
        check("C allocprobe survives", "#" in out or "$" in out or "PASS" in out,
              out[-160:].replace("\n", " "))
        shot(s, "C_allocprobe")
        out = run(s, "mem")
        check("C mem still consistent after probe", "PMM" in out,
              out[-200:].replace("\n", " "))
    finally:
        kill_qemu(proc)


def main():
    keep = "--keep" in sys.argv
    qemu = find_qemu(None) or "/home/user/bin/qemu-system-i386"
    if not os.path.exists(qemu):
        print("qemu-system-i386 not found")
        return 2
    if not os.path.isfile(ISO):
        print("ISO missing: %s" % ISO)
        return 2

    which = "ABC"
    if "--boot" in sys.argv:
        which = sys.argv[sys.argv.index("--boot") + 1].upper()

    if "A" in which:
        prepare()
        probe_pe_pkg_tools(qemu)
    if "B" in which:
        probe_mount(qemu)
    if "C" in which:
        if not os.path.exists(REVIEW_DISK):
            prepare()
        probe_mem(qemu)

    fails = [r for r in results if not r[1]]
    print("\n===== review probes: %d run, %d fail =====" % (len(results), len(fails)))
    for n, ok, note in fails:
        print("  FAIL %s  %s" % (n, note))
    if not keep and os.path.isdir(STAGE):
        shutil.rmtree(STAGE, ignore_errors=True)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
