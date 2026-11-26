#!/usr/bin/env python3
"""pe_reloc_probe.py -- P1-1 (D9) PE base-relocation on-metal probe.

Two cases against a freshly built ISO, using a two-pass disk like
p1_pe_check.py (format the FAT volume, stop, inject the hand-built PEs,
boot again):

  reloc     tests/mkpe_reloc.py --sample: preferred ImageBase 0x400000
            (outside the user arena), one HIGHLOW(3) fixup on an absolute
            message pointer.  A correct relocation prints the marker
            "PE-RELOC PROBE OK"; a missing/incorrect fixup dereferences
            the unmapped preferred base.

  badreloc  tests/mkpe_reloc.py --badreloc: the .reloc block claims 64
            entries but only two exist.  The loader must return a negative
            error and keep running -- no ring0 fault / system halted.

Usage:
  python tests/pe_reloc_probe.py [catos.iso] [--baseline] [--shot-dir DIR]

  --baseline : record the pre-fix behaviour as an observation (reloc marker
               expected absent); used for the before/after comparison only.
"""
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
sys.path.insert(0, HERE)

from qemu_driver import (kill_qemu, cmd, find_qemu, force_text, screen_text,
                         sendkey, type_text, wait_for_prompt)
from qemu_regress import ppm_to_png

PORT = 4463
DISK = os.path.join(HERE, "qemu_disk_reloc.img")
STAGE = os.path.join(tempfile.gettempdir(), "catos_p11_stage")
STAGE_DISK = os.path.join(STAGE, "qemu_disk_reloc.img")
ART = os.path.join(ROOT, "artifacts", "v282", "p1-1")

PE_SAMPLE = "PELOC.EXE"
PE_BAD = "PELOCBAD.EXE"
MARKER = "PE-RELOC PROBE OK"
CRASH_MARKERS = ("page fault", "kernel fault", "system halted")


def boot_direct(qemu, iso, port, disk):
    log = os.path.join(HERE, "pe_reloc_qemu.log")
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
            return proc, socket.create_connection(("127.0.0.1", port), timeout=5)
        except OSError:
            time.sleep(2)
    kill_qemu(proc)
    raise RuntimeError("no monitor on port %d" % port)


def build_artifacts():
    os.makedirs(ART, exist_ok=True)
    sample = os.path.join(ART, PE_SAMPLE)
    bad = os.path.join(ART, PE_BAD)
    subprocess.run([sys.executable, os.path.join(HERE, "mkpe_reloc.py"),
                    "--sample", sample], check=True)
    subprocess.run([sys.executable, os.path.join(HERE, "mkpe_reloc.py"),
                    "--badreloc", bad], check=True)
    return sample, bad


def fresh_disk():
    for p in (DISK, STAGE_DISK):
        if os.path.exists(p):
            os.remove(p)
    os.makedirs(STAGE, exist_ok=True)
    with open(DISK, "wb") as f:
        f.truncate(64 * 1024 * 1024)
    shutil.copy2(DISK, STAGE_DISK)


def stage_back():
    shutil.copy2(STAGE_DISK, DISK)


def inject(src, name):
    rc = subprocess.run([sys.executable, os.path.join(HERE, "inject_any.py"),
                         DISK, src, name],
                        capture_output=True, text=True)
    if rc.returncode != 0:
        print("inject FAIL %s: %s %s" % (name, rc.stdout, rc.stderr))
        sys.exit(3)
    print("inject OK %s" % name)


def shoot(s, name):
    os.makedirs(ART, exist_ok=True)
    # The workspace fs does not reliably honour QEMU's file creation, so
    # dump to /tmp first and convert into the artifact dir (same staging
    # workaround as qemu_driver._stage_image).
    stage = os.path.join(tempfile.gettempdir(), "catos_p11_shots")
    os.makedirs(stage, exist_ok=True)
    ppm = os.path.join(stage, name + ".ppm")
    png = os.path.join(ART, name + ".png")
    if os.path.exists(ppm):
        os.remove(ppm)
    reply = cmd(s, "screendump %s" % ppm.replace("\\", "/"), 1.0)
    for _ in range(30):
        if os.path.exists(ppm) and os.path.getsize(ppm) > 0:
            break
        time.sleep(0.5)
    if os.path.exists(ppm) and os.path.getsize(ppm) > 0:
        ppm_to_png(ppm, png)      # ppm_to_png removes the PPM itself
    else:
        print("[shot] WARNING: no PPM for %s (monitor: %s)"
              % (name, reply.strip()[:120]))
    return png


def crashed(txt):
    low = txt.lower()
    return any(m in low for m in CRASH_MARKERS)


def main():
    args = sys.argv[1:]
    iso = None
    baseline = False
    while args:
        a = args.pop(0)
        if a == "--baseline":
            baseline = True
        elif a == "--shot-dir":
            global ART
            ART = args.pop(0)
        elif not a.startswith("--"):
            iso = a
    iso = os.path.abspath(iso or os.path.join(ROOT, "kernel_v25", "catos.iso"))
    qemu = find_qemu() or "/home/user/bin/qemu-system-i386"
    if not os.path.exists(qemu):
        print("qemu-system-i386 not found")
        return 2
    if not os.path.isfile(iso):
        print("ISO not found: %s" % iso)
        return 2

    sample, bad = build_artifacts()

    # ---- pass 1: format the volume ----
    fresh_disk()
    proc, s = boot_direct(qemu, iso, PORT, disk=STAGE_DISK)
    print("monitor connected (pass 1: format)")
    force_text(s)
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

    with open(DISK, "rb") as f:
        sig = f.read(512)
    if sig[510] != 0x55 or sig[511] != 0xAA:
        print("FATAL: disk volume did not survive pass 1")
        return 3

    inject(sample, PE_SAMPLE)
    inject(bad, PE_BAD)

    # ---- pass 2: exercise the cases ----
    shutil.copy2(DISK, STAGE_DISK)
    proc, s = boot_direct(qemu, iso, PORT, disk=STAGE_DISK)
    print("monitor connected (pass 2: acceptance)")
    force_text(s)
    if not wait_for_prompt(s):
        print("FATAL: prompt never appeared in pass 2")
        kill_qemu(proc)
        return 3

    results = []

    def check(name, ok, note):
        results.append((name, ok))
        print("%-4s %-12s %s" % ("PASS" if ok else "FAIL", name, note))

    def run_pe(name):
        """`run` the PE, answer N to the convert-to-CE offer, return screen."""
        type_text(s, "run /bin/%s" % name)
        sendkey(s, "ret", 2.0)
        time.sleep(1.0)
        txt = screen_text(s)
        if "Convert to CE01" in txt or "MZ" in txt:
            sendkey(s, "n", 0.4)
            sendkey(s, "ret", 3.0)
        return screen_text(s)

    try:
        # Case 1: reloc fixup must produce the marker line.
        txt = run_pe(PE_SAMPLE)
        print("[reloc screen tail] %s" % txt[-220:].replace("\n", " | "))
        rel_ok = MARKER in txt
        if crashed(txt):
            check("reloc", False, "kernel fault: %s" % txt[-200:])
        elif rel_ok:
            check("reloc", True, "marker: %s" % MARKER)
        else:
            check("reloc", baseline, "marker absent (baseline observation)"
                  if baseline else "marker absent: %s" % txt[-200:])
        shoot(s, "p1-1-reloc" + ("-baseline" if baseline else ""))

        # Case 2: malformed reloc must be rejected, guest keeps running.
        type_text(s, "clear")
        sendkey(s, "ret", 0.6)
        txt = run_pe(PE_BAD)
        print("[badreloc screen tail] %s" % txt[-220:].replace("\n", " | "))
        bad_crash = crashed(txt)
        # wait briefly for the shell prompt to come back
        time.sleep(1.0)
        txt2 = screen_text(s)
        prompt_back = wait_for_prompt(s, timeout=10.0)
        check("badreloc", not bad_crash and prompt_back,
              "no fault, prompt back" if not bad_crash else
              "kernel fault: %s" % txt2[-200:])
        shoot(s, "p1-1-badreloc" + ("-baseline" if baseline else ""))
    finally:
        kill_qemu(proc)

    fails = [n for n, ok in results if not ok]
    print("\nsteps=%d ok=%d fail=%d" % (len(results),
                                        len(results) - len(fails), len(fails)))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
