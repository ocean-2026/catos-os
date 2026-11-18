#!/usr/bin/env python3
"""shell_guess_regress.py -- v2.47 pending-guess `y` one-shot replay.

When the main prompt offers a nearest-command suggestion after an unknown
command, the next bare `y`/`Y` replays that suggested command line; any
other input consumes the offer.  In-command y/n prompts (PE->CE offer,
format gate, gfx consent, wizard) read their own input and must never
trigger this replay.

Coverage:
  1. bare `y` replays the last suggested command        (normal replay)
  2. uppercase `Y` also replays
  3. original args are carried to the guess; no residue after replay
  4. with no pending, `y` is an ordinary unknown command
  5. PE->CE offer's own y answer never fires the pending replay
  6. a second wrong input overwrites the pending guess

Two passes (same pattern as p1_pe_check.py): pass 1 formats the volume,
the sample PE is injected into /BIN, pass 2 exercises the shell flow.

Usage:
  python tests/shell_guess_regress.py [--qemu PATH] [--port N] [--keep]
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

from qemu_driver import (kill_qemu, cmd, find_qemu, screen_text, sendkey,
                         type_text, wait_for_prompt, force_text)

PORT = 4450
DISK = os.path.join(HERE, "qemu_disk_guess.img")
STAGE = os.path.join(tempfile.gettempdir(), "catos_guess_stage")
STAGE_DISK = os.path.join(STAGE, "qemu_disk_guess.img")
SHOTS = os.path.join(HERE, "qemu_shots_guess")
ART = os.path.join(HERE, "guess_artifacts")
ISO = os.path.join(ROOT, "kernel_v25", "catos.iso")

PE_NAME = "PESAMPLE.EXE"
PE_PATH = os.path.join(ART, PE_NAME)


def boot_direct(qemu, port, disk):
    """Launch QEMU against a local /tmp staged disk (workspace fs flock)."""
    log = os.path.join(HERE, "guess_qemu.log")
    cmdline = [qemu, "-m", "256", "-no-reboot", "-net", "none",
               "-cdrom", ISO, "-boot", "order=d", "-display", "none",
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


def inject():
    rc = subprocess.run([sys.executable, os.path.join(HERE, "inject_any.py"),
                          DISK, PE_PATH, PE_NAME],
                         capture_output=True, text=True)
    if rc.returncode != 0:
        print("inject FAIL %s: %s %s" % (PE_NAME, rc.stdout, rc.stderr))
        sys.exit(3)
    print("inject OK %s" % PE_NAME)


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
    args = [a for a in sys.argv[1:] if a not in ("--keep",)]
    iso = None
    if args and not args[0].startswith("--"):
        iso = os.path.abspath(args.pop(0))
    qemu = None
    if "--qemu" in args:
        qemu = args[args.index("--qemu") + 1]
    port = PORT
    if "--port" in args:
        port = int(args[args.index("--port") + 1])
    if iso is None:
        iso = ISO
    qemu = find_qemu(qemu) or "/home/user/bin/qemu-system-i386"
    if not os.path.exists(qemu):
        print("qemu-system-i386 not found")
        return 2

    os.makedirs(ART, exist_ok=True)
    os.makedirs(SHOTS, exist_ok=True)
    subprocess.run([sys.executable, os.path.join(HERE, "mkpe.py"),
                    "--sample", PE_PATH], check=True)

    # ---- pass 1: format the volume ----
    fresh_disk()
    proc, s = boot_direct(qemu, port, disk=STAGE_DISK)
    print("monitor connected (pass 1: format)")
    force_text(s)          # v2.56: boot defaults to gfx24; text suite
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

    inject()

    # ---- pass 2: exercise the pending-guess flow ----
    shutil.copy2(DISK, STAGE_DISK)
    proc, s = boot_direct(qemu, port, disk=STAGE_DISK)
    print("monitor connected (pass 2: guess replay)")
    force_text(s)          # v2.56: boot defaults to gfx24; text suite
    if not wait_for_prompt(s):
        print("FATAL: prompt never appeared in pass 2")
        kill_qemu(proc)
        return 3

    results = []

    def check(name, ok, note=""):
        results.append((name, ok, note))
        print("%-4s %-26s %s" % ("PASS" if ok else "FAIL", name, note))

    def do(line, wait=1.5):
        type_text(s, line)
        sendkey(s, "ret", wait)

    def txt():
        return screen_text(s)

    # Case 1: bare `y` replays the suggested command.  `dat` -> `date`
    # (one-line output, so the `-> date` echo stays on screen).
    do("clear", 1.0)
    do("dat", 1.5)
    t = txt()
    check("1a: suggest date for `dat`", "did you mean: date" in t, t[:120])
    do("y", 2.0)
    t = txt()
    check("1b: `y` replays `date`", "-> date" in t, t[:120])

    # Case 2: uppercase `Y` replays as well.  `who` -> `whoami`.
    do("who", 1.5)
    t = txt()
    check("2a: suggest whoami for `who`", "did you mean: whoami" in t, t[:120])
    do("Y", 2.0)
    t = txt()
    check("2b: `Y` replays `whoami`", "-> whoami" in t, t[:120])

    # Case 3: original args are carried to the guess.  `lx /bin` replays as
    # `ls /bin`; the /bin listing (not the root listing) proves the arg
    # travelled with the replay.  The `-> ls /bin` echo line scrolls off a
    # long listing, so this case asserts on the listing contents instead.
    do("lx /bin", 1.5)
    t = txt()
    check("3a: suggest ls for `lx /bin`", "did you mean: ls" in t, t[:120])
    do("y", 2.0)
    t = txt()
    check("3b: replay carries `/bin` arg", "pesample.exe" in t, t[:120])

    # Case 4: pending consumed -> a bare `y` is an ordinary unknown command.
    do("y", 1.5)
    t = txt()
    check("4a: no pending -> `y` unknown", "unknown command: y" in t, t[:120])
    check("4b: no stale replay after consume", "-> ls" not in t, t[:120])
    do("clear", 1.0)

    # Case 5: PE->CE offer's own y must follow the original path, not replay.
    do("lx", 1.5)
    t = txt()
    check("5a: pending armed before PE offer", "did you mean: ls" in t, t[:120])
    do("run /bin/%s" % PE_NAME, 2.0)
    time.sleep(1.0)
    t = txt()
    check("5b: PE conversion offer shown", "Convert to CE01" in t, t[:120])
    sendkey(s, "y", 0.4)
    sendkey(s, "ret", 2.0)
    # second offer: save CE as [default name] -- Enter accepts the default
    sendkey(s, "ret", 3.0)
    t = txt()
    check("5c: offer y followed conversion path", "converted to" in t, t[:120])
    check("5d: offer y did NOT replay guess", "-> ls" not in t, t[:120])

    # Case 6: a second wrong input overwrites the pending guess.
    do("clear", 1.0)
    do("dat", 1.5)
    t = txt()
    check("6a: first suggest date", "did you mean: date" in t, t[:120])
    do("who", 1.5)
    t = txt()
    check("6b: second suggest whoami overwrites", "did you mean: whoami" in t, t[:120])
    do("y", 2.0)
    t = txt()
    check("6c: replay uses overwritten guess", "-> whoami" in t, t[:120])
    check("6d: stale first guess not replayed", "-> date" not in t, t[:120])

    shot(s, "final")

    if not keep:
        kill_qemu(proc)

    fails = [r for r in results if not r[1]]
    print("\nsteps=%d ok=%d fail=%d shots in %s"
          % (len(results), len(results) - len(fails), len(fails), SHOTS))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
