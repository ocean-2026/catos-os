#!/usr/bin/env python3
"""p1_pe_check.py -- P1 PE/EXE support end-to-end acceptance driver.

Covers the three P1 paths against a real QEMU/i386 guest:
  A. shell `run` on an MZ/PE32 file pops the convert-to-CE01 offer
  B. kernel spawn_load_pe() loads the PE32 EXE directly and runs it
  C. tests/mkpe.py translates the PE32 into a CE01 image that boots in
     the guest

Flow (two passes, like go_regress.py):
  pass 1: boot the ISO on a fresh 64 MiB disk, `format` the FAT volume,
          stop the guest, inject the sample PE + converted CE into /BIN
  pass 2: boot again, exercise the three paths, screenshot each step

Note: qemu_driver._stage_image() copies the disk to a temp dir and
discards the copy on process exit, so this driver launches QEMU
directly against the original disk file to keep the FAT volume across
the two passes.

Usage:
  python tests/p1_pe_check.py [--qemu PATH] [--keep]
"""
import os
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
sys.path.insert(0, HERE)

from qemu_driver import (kill_qemu, cmd, find_qemu, read_screen,
                         screen_text, sendkey, type_text, wait_for_prompt)

PORT = 4710
DISK = os.path.join(HERE, "qemu_disk_p1.img")
# The workspace fs does not honour QEMU's image flock reliably, so stage
# the disk on a local /tmp dir (same workaround as qemu_driver._stage_image)
# and copy it back to the real disk between passes.
import tempfile
STAGE = os.path.join(tempfile.gettempdir(), "catos_p1_stage")
STAGE_DISK = os.path.join(STAGE, "qemu_disk_p1.img")
SHOTS = os.path.join(HERE, "p1_shots")
ART = os.path.join(HERE, "p1_artifacts")
ISO = os.path.join(ROOT, "kernel_v25", "catos.iso")

PE_NAME = "PESAMPLE.EXE"
CE_NAME = "PESAMPLE.CE"
PE_PATH = os.path.join(ART, PE_NAME)
CE_PATH = os.path.join(ART, CE_NAME)

EXPECT_MSG = "PE->CE roundtrip OK"


def boot_direct(qemu, iso, port, disk):
    """Launch QEMU against a local /tmp staged disk (workspace fs flock)."""
    log = os.path.join(HERE, "p1_qemu.log")
    cmdline = [qemu, "-m", "256", "-no-reboot", "-net", "none",
               "-cdrom", iso, "-boot", "order=d", "-display", "none",
               "-monitor", "tcp:127.0.0.1:%d,server,nowait" % port]
    if disk:
        cmdline += ["-drive", "file=%s,format=raw,if=ide" % disk]
    err = open(log, "wb")
    proc = subprocess.Popen(cmdline, stdout=subprocess.DEVNULL, stderr=err)
    for _ in range(40):
        if proc.poll() is not None:
            err.flush()
            raise RuntimeError("qemu exited rc=%r (see %s)" % (proc.returncode, log))
        try:
            return proc, socket.create_connection(("127.0.0.1", port),
                                                  timeout=5)
        except OSError:
            time.sleep(2)
    kill_qemu(proc)
    raise RuntimeError("no monitor on port %d" % port)


def prep_artifacts():
    os.makedirs(ART, exist_ok=True)
    os.makedirs(SHOTS, exist_ok=True)
    subprocess.run([sys.executable, os.path.join(HERE, "mkpe.py"),
                    "--sample", PE_PATH], check=True)
    subprocess.run([sys.executable, os.path.join(HERE, "mkpe.py"),
                    PE_PATH, CE_PATH], check=True)


def fresh_disk():
    import shutil
    for p in (DISK, STAGE_DISK):
        if os.path.exists(p):
            os.remove(p)
    os.makedirs(STAGE, exist_ok=True)
    with open(DISK, "wb") as f:
        f.truncate(64 * 1024 * 1024)
    shutil.copy2(DISK, STAGE_DISK)


def stage_back():
    import shutil
    shutil.copy2(STAGE_DISK, DISK)


def inject():
    for src, name in ((PE_PATH, PE_NAME), (CE_PATH, CE_NAME)):
        rc = subprocess.run([sys.executable, os.path.join(HERE, "inject_any.py"),
                             DISK, src, name],
                            capture_output=True, text=True)
        if rc.returncode != 0:
            print("inject FAIL %s: %s %s" % (name, rc.stdout, rc.stderr))
            sys.exit(3)
        print("inject OK %s" % name)


def shot(s, name):
    png = os.path.join(SHOTS, name + ".png")
    ppm = os.path.join(SHOTS, name + ".ppm")
    if os.path.exists(ppm):
        os.remove(ppm)
    cmd(s, "screendump %s" % ppm.replace("\\", "/"), 0.5)
    for _ in range(40):
        if os.path.exists(ppm) and os.path.getsize(ppm) > 0:
            break
        time.sleep(0.5)
    else:
        raise RuntimeError("no screendump for %s" % name)
    from qemu_regress import ppm_to_png
    ppm_to_png(ppm, png)
    return png


def main():
    keep = "--keep" in sys.argv
    qemu = None
    if "--qemu" in sys.argv:
        qemu = sys.argv[sys.argv.index("--qemu") + 1]
    qemu = find_qemu(qemu) or "/home/user/bin/qemu-system-i386"
    if not os.path.exists(qemu):
        print("qemu-system-i386 not found")
        return 2

    prep_artifacts()

    # ---- pass 1: format the volume ----
    fresh_disk()
    proc, s = boot_direct(qemu, ISO, PORT, disk=STAGE_DISK)
    print("monitor connected (pass 1: format)")
    if not wait_for_prompt(s):
        print("FATAL: prompt never appeared")
        kill_qemu(proc)
        return 3
    type_text(s, "format")
    sendkey(s, "ret", 1.0)
    sendkey(s, "y", 0.4)
    sendkey(s, "ret", 12.0)
    kill_qemu(proc)
    time.sleep(2)
    stage_back()

    # sanity: the FAT signature must persist in the original disk file
    with open(DISK, "rb") as f:
        sig = f.read(512)
    if sig[510] != 0x55 or sig[511] != 0xAA:
        print("FATAL: disk volume did not survive pass 1")
        return 3

    inject()

    # ---- pass 2: exercise paths A/B/C ----
    import shutil
    shutil.copy2(DISK, STAGE_DISK)
    proc, s = boot_direct(qemu, ISO, PORT, disk=STAGE_DISK)
    print("monitor connected (pass 2: acceptance)")
    if not wait_for_prompt(s):
        print("FATAL: prompt never appeared in pass 2")
        kill_qemu(proc)
        return 3

    results = []

    def check(name, ok, note=""):
        results.append((name, ok, note))
        print("%-4s %-22s %s" % ("PASS" if ok else "FAIL", name, note))

    # Path A: `run` the MZ/PE file -> conversion offer appears on screen.
    type_text(s, "run /bin/%s" % PE_NAME)
    sendkey(s, "ret", 2.0)
    time.sleep(1.5)
    shot(s, "01_mz_convert_prompt")
    txt = screen_text(s)
    check("A: MZ convert prompt", "Convert to CE01" in txt, txt[:120])

    # Answer N: skip conversion, fall through to the direct PE load (B).
    sendkey(s, "n", 0.4)
    sendkey(s, "ret", 3.0)
    shot(s, "02_pe_direct_run")
    txt = screen_text(s)
    check("B: direct PE32 run", EXPECT_MSG in txt, txt[:120])

    # Path C: the offline-translated CE must run in the guest (before A's
    # in-kernel conversion overwrites the injected offline CE).
    type_text(s, "run /bin/%s" % CE_NAME)
    sendkey(s, "ret", 3.0)
    shot(s, "04_offline_translated_ce_run")
    txt = screen_text(s)
    check("C: mkpe.py CE runs", EXPECT_MSG in txt, txt[:120])

    # Path A again: answer Y, convert in-kernel to the default .ce name.
    type_text(s, "run /bin/%s" % PE_NAME)
    sendkey(s, "ret", 2.0)
    time.sleep(1.0)
    shot(s, "03b_convert_offer")
    sendkey(s, "y", 0.4)
    sendkey(s, "ret", 2.0)
    # second prompt: save CE as [default] -- Enter accepts the default name
    sendkey(s, "ret", 3.0)
    shot(s, "03c_converted_ce_run")
    txt = screen_text(s)
    check("A: in-kernel conversion saved", "converted to" in txt, txt[:120])

    # the freshly converted CE must run in the guest
    type_text(s, "run /bin/%s" % CE_NAME)
    sendkey(s, "ret", 3.0)
    shot(s, "03d_converted_ce_run")
    txt = screen_text(s)
    check("A: converted CE runs", EXPECT_MSG in txt, txt[:120])

    # inventory screenshot for the report
    type_text(s, "ls /bin")
    sendkey(s, "ret", 2.0)
    shot(s, "05_ls_bin")

    if not keep:
        kill_qemu(proc)

    fails = [r for r in results if not r[1]]
    print("\nsteps=%d ok=%d fail=%d shots in %s"
          % (len(results), len(results) - len(fails), len(fails), SHOTS))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
