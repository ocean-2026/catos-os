#!/usr/bin/env python3
"""review_round2.py -- second-pass functional probes for the v2.44 review.

Closes out the test-side false negatives left by night_review_probe.py:

  A  help surface: scroll the console BACK (Ctrl+Up) so the top-of-help
     rows (cowsay/fortune/pkg/head ...) leave the live grid and are read
     from the scrollback archive, instead of the visible-only snapshot.
     TAB completion: cls / su aliases now live in sh_cmds[].
  B  mount matrix: `clear` before umount/df listings so retained scrollback
     text cannot pollute "dir empty" / "no /docs row" assertions.
  C  3840 MiB: PMM free-frame count before vs after allocprobe (task teardown
     must return its arena frames); hwinfo keyword check fixed to the
     actual lower-case labels.
  D  256 MiB: same allocprobe before/after consistency on the regular tier.

Usage:
  python3 tests/review_round2.py [--boot ABCD]
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qemu_driver import (find_qemu, kill_qemu, screen_text, sendkey,
                         type_text, wait_for_prompt)  # noqa: E402
from qemu_regress import ppm_to_png, shot  # noqa: E402

REPO = os.path.dirname(HERE)
KERNEL = os.path.join(REPO, "kernel_v25")
ISO = os.path.join(KERNEL, "catos.iso")
SRC_DISK = os.path.join(KERNEL, "catos_disk.img")
PART_DISK = os.path.join(HERE, "qemu_disk_part.img")
BASE_DISK = os.path.join(HERE, "qemu_disk.img")
ART = os.path.join(HERE, "review_assets")
STAGE = os.path.join(tempfile.gettempdir(), "catos_round2_stage")
SHOTS = os.path.join(HERE, "qemu_shots_review")
os.makedirs(SHOTS, exist_ok=True)

REVIEW_DISK = os.path.join(STAGE, "review_disk.img")
results = []


def check(name, ok, note=""):
    results.append((name, bool(ok), note))
    print("  %-4s %-34s %s" % ("PASS" if ok else "FAIL", name, note[:150]))


def boot(qemu, disk, port, mem="256"):
    os.makedirs(STAGE, exist_ok=True)
    stage_disk = os.path.join(STAGE, os.path.basename(disk))
    if os.path.abspath(disk) != os.path.abspath(stage_disk):
        shutil.copy2(disk, stage_disk)
    cmdline = [qemu, "-m", mem, "-no-reboot", "-net", "none",
               "-cdrom", ISO, "-boot", "order=d", "-display", "none",
               "-monitor", "tcp:127.0.0.1:%d,server,nowait" % port,
               "-drive", "file=%s,format=raw,if=ide" % stage_disk]
    proc = subprocess.Popen(cmdline, stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL)
    import socket
    for _ in range(40):
        if proc.poll() is not None:
            raise RuntimeError("qemu exited rc=%r" % proc.returncode)
        try:
            return proc, socket.create_connection(("127.0.0.1", port), timeout=5)
        except OSError:
            time.sleep(1)
    kill_qemu(proc)
    raise RuntimeError("no monitor")


def run(s, line, wait=1.3):
    type_text(s, line)
    sendkey(s, "ret", wait)
    time.sleep(0.4)
    return screen_text(s)


def prep_review_disk():
    os.makedirs(STAGE, exist_ok=True)
    subprocess.run([sys.executable, os.path.join(ART, "gen_bad_pe.py"), ART],
                   check=True)
    if os.path.exists(REVIEW_DISK):
        os.remove(REVIEW_DISK)
    shutil.copy2(BASE_DISK, REVIEW_DISK)
    for n in ("BADMZ.EXE", "BADA_LF.EXE", "TRUNC_PE.EXE", "OVERSEC.EXE"):
        rc = subprocess.run([sys.executable, os.path.join(HERE, "inject_any.py"),
                              REVIEW_DISK, os.path.join(ART, n), n],
                             capture_output=True, text=True)
        if rc.returncode != 0:
            raise RuntimeError("inject %s failed: %s" % (n, rc.stderr))
    subprocess.run([sys.executable, os.path.join(HERE, "mkpe.py"),
                    "--sample", os.path.join(ART, "PESAMPLE.EXE")], check=True)
    rc = subprocess.run([sys.executable, os.path.join(HERE, "inject_any.py"),
                         REVIEW_DISK, os.path.join(ART, "PESAMPLE.EXE"),
                         "PESAMPLE.EXE"], capture_output=True, text=True)
    if rc.returncode != 0:
        raise RuntimeError("inject PESAMPLE failed: %s" % rc.stderr)


def free_frames(text):
    m = re.search(r"free\s+(\d+)\s+frames", text)
    return int(m.group(1)) if m else None


# ---------------------------------------------------------------- Boot A
def probe_help_completion(qemu):
    print("\n=== A: help scrollback + TAB aliases (256 MiB) ===")
    prep_review_disk()
    proc, s = boot(qemu, REVIEW_DISK, 4771)
    try:
        if not wait_for_prompt(s):
            check("A boot prompt", False); return
        run(s, "clear", 0.8)

        run(s, "help", 1.0)
        # scroll back through the archive, accumulating every visible frame
        archive = screen_text(s)
        for _ in range(14):
            sendkey(s, "ctrl-up", 0.25)
            time.sleep(0.15)
            archive += " " + screen_text(s)
        sendkey(s, "ctrl-down", 0.3)
        for tok in ("cowsay", "fortune", "pkg", "head", "tail", "wc",
                    "tree", "mount", "umount", "df", "cal", "yes", "sl"):
            check("help archive lists %s" % tok, tok in archive, "")
        shot(s, "R2_help_archive")

        # TAB: "cl" matches clear + cls; "su" matches su + sudo
        run(s, "clear", 0.8)
        type_text(s, "cl")
        sendkey(s, "tab", 0.8)
        out = screen_text(s)
        check("TAB completes cls alias", "cls" in out, out[-120:].replace("\n", " "))
        sendkey(s, "ret", 0.5)

        type_text(s, "su")
        sendkey(s, "tab", 0.8)
        out = screen_text(s)
        check("TAB completes su alias", "su" in out, out[-120:].replace("\n", " "))
        sendkey(s, "ret", 0.5)
        shot(s, "R2_completion_aliases")
    finally:
        kill_qemu(proc)


# ---------------------------------------------------------------- Boot B
def probe_mount_clean(qemu):
    print("\n=== B: mount matrix, cleared pre-umount/df (256 MiB) ===")
    proc, s = boot(qemu, PART_DISK, 4772)
    try:
        if not wait_for_prompt(s):
            check("B boot prompt", False); return
        run(s, "clear", 0.8)

        out = run(s, "mount 2 /docs")
        check("B mount /docs", "mounted" in out, out[-120:].replace("\n", " "))
        run(s, "cp /bin/hello.ce /docs/hello.ce", 1.2)

        run(s, "umount /docs", 1.0)
        run(s, "clear", 0.8)
        out = run(s, "ls /docs")
        check("B dir empty after umount", "hello.ce" not in out,
              out[-120:].replace("\n", " "))

        run(s, "mount 2 /docs", 1.0)
        run(s, "clear", 0.8)
        out = run(s, "df")
        check("B df still lists /docs", "/docs" in out,
              out[-160:].replace("\n", " "))

        run(s, "umount /docs", 1.0)
        run(s, "clear", 0.8)
        out = run(s, "df")
        check("B df row gone after umount", "/docs" not in out,
              out[-160:].replace("\n", " "))
        shot(s, "R2_df_after_umount")

        # corrupted/absent package name paths
        out = run(s, "pkg remove nosuchpkg")
        check("B pkg remove unknown name", "no such package" in out,
              out[-120:].replace("\n", " "))
    finally:
        kill_qemu(proc)


# ---------------------------------------------------------------- Boot C/D
def probe_mem(qemu, mem, port, tag):
    print("\n=== %s: allocprobe before/after PMM consistency (%s MiB) ==="
          % (tag, mem))
    proc, s = boot(qemu, REVIEW_DISK, port, mem)
    try:
        if not wait_for_prompt(s):
            check("%s boot prompt" % tag, False); return
        run(s, "clear", 0.8)
        before = free_frames(run(s, "mem"))
        check("%s baseline free frames read" % tag, before is not None,
              "free=%s" % before)

        out = run(s, "run /bin/allocprobe.ce", 3.0)
        check("%s allocprobe PASS" % tag, "ALLOCPROBE PASS" in out,
              out[-120:].replace("\n", " "))
        shot(s, "R2_%s_allocprobe" % tag)

        after = free_frames(run(s, "mem"))
        check("%s free frames reclaimed after exit" % tag,
              after is not None and after >= (before or 0) - 2,
              "before=%s after=%s" % (before, after))
        out = screen_text(s)
        check("%s mem view consistent" % tag, "PMM" in out,
              out[-160:].replace("\n", " "))

        if mem == "3840":
            hw = run(s, "hwinfo")
            check("%s hwinfo dumps hardware" % tag,
                  ("cpu.mhz" in hw or "mem.total" in hw or "disk.model" in hw
                   or "pci devices" in hw),
                  hw[-120:].replace("\n", " "))
            shot(s, "R2_%s_hwinfo" % tag)
    finally:
        kill_qemu(proc)


def main():
    qemu = find_qemu(None) or "/home/user/bin/qemu-system-i386"
    which = "ABCD"
    if "--boot" in sys.argv:
        which = sys.argv[sys.argv.index("--boot") + 1].upper()
    if "A" in which:
        probe_help_completion(qemu)
    if "B" in which:
        probe_mount_clean(qemu)
    if "C" in which:
        if not os.path.exists(REVIEW_DISK):
            prep_review_disk()
        probe_mem(qemu, "3840", 4773, "C")
    if "D" in which:
        if not os.path.exists(REVIEW_DISK):
            prep_review_disk()
        probe_mem(qemu, "256", 4774, "D")

    fails = [r for r in results if not r[1]]
    print("\n===== round2 probes: %d run, %d fail =====" % (len(results), len(fails)))
    for n, ok, note in fails:
        print("  FAIL %s  %s" % (n, note))
    shutil.rmtree(STAGE, ignore_errors=True)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
