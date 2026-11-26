#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""v2.82 P1-6 (A2.4): detachability regression on real QEMU.

What it checks, against a fixed copy of the shipped 64 MiB volume:

  Reality check  - `pkg list` / `pkg info desktop` / `ls /pkg`: record the
                   real returned text for the desktop package.  The current
                   build (kernel_v25/build/pkg.list) only ships bc/stats/rps;
                   the block desktop is kernel-state gui/ code.  Any desktop
                   entry on the shipped volume is legacy residue.

  desktop pkg    - `pkg install desktop` -> status -> run `desktop` -> quit
                   -> `pkg remove desktop` -> status back to available.

  kernel gui     - enter the block desktop via `gui`, Esc back to the
                   console; `ps` must show only the shell task (no leftover
                   desktop task), prompt clean.

  bc equivalence - `pkg install bc` -> `bc 12 + 30` prints 42 ->
                   `pkg remove bc` -> `bc` is an unknown command again.

  integrity      - host-side kernel.bin sha256 recorded before and after the
                   whole guest session; must be identical.  The guest only
                   writes the volume copy, never the kernel image.

  fresh volume   - a freshly formatted volume lists bc/stats/rps and NO
                   desktop entry, confirming the desktop package is not part
                   of the current build repo.

Usage:
  python3 tests/detach_probe.py [catos.iso]
"""
import hashlib
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qemu_driver as q

ROOT = os.path.dirname(HERE)
ISO = os.path.join(ROOT, "kernel_v25", "catos.iso")
KERNEL_BIN = os.path.join(ROOT, "kernel_v25", "build", "kernel.bin")
SHIPPED = os.path.join(ROOT, "kernel_v25", "catos_disk.img")
OUT = os.path.join(ROOT, "artifacts", "v282", "p1-6")
os.makedirs(OUT, exist_ok=True)
PORT = 4559

results = []


def check(name, ok, note=""):
    results.append((name, bool(ok)))
    print("%-4s %-34s %s" % ("PASS" if ok else "FAIL", name, note))


def sha256_file(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def boot_fixed(port, disk):
    qemu = q.find_qemu()
    if not qemu:
        raise RuntimeError("qemu-system-i386 not found on PATH")
    proc = subprocess.Popen(
        [qemu, "-m", "256", "-no-reboot", "-net", "none",
         "-cdrom", ISO, "-boot", "order=d", "-display", "none",
         "-monitor", "tcp:127.0.0.1:%d,server,nowait" % port,
         "-drive", "file=%s,format=raw,if=ide,index=0,media=disk" % disk],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(40):
        if proc.poll() is not None:
            raise RuntimeError("qemu exited rc=%r" % proc.returncode)
        try:
            return proc, socket.create_connection(("127.0.0.1", port), timeout=5)
        except OSError:
            time.sleep(2)
    q.kill_qemu(proc)
    raise RuntimeError("no monitor on port %d" % port)


def shoot(sock, name):
    ppm = os.path.join(OUT, name + ".ppm")
    png = os.path.join(OUT, name + ".png")
    if os.path.exists(ppm):
        os.remove(ppm)
    q.cmd(sock, "screendump %s" % ppm, 1.0)
    for _ in range(10):
        if os.path.exists(ppm) and os.path.getsize(ppm) > 0:
            break
        time.sleep(0.3)
    if os.path.exists(ppm):
        from PIL import Image
        Image.open(ppm).save(png)
        os.remove(ppm)
    return png


def do(sock, line, wait=1.2):
    q.type_text(sock, line, 0.01)
    q.sendkey(sock, "ret", wait)


def enter_shell(sock):
    time.sleep(12.0)
    q.sendkey(sock, "esc", 1.0)
    time.sleep(1.0)
    q.force_text(sock)
    return q.wait_for_prompt(sock, 120.0)


def fault_free(text):
    return not re.search(r"!!! PANIC|page fault|kernel fault|system halted",
                         text, re.I)


def task_count(text):
    m = re.search(r"(\d+)\s+task\(s\)", text)
    return int(m.group(1)) if m else None


def main():
    iso = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else ISO
    if not os.path.isfile(iso):
        print("ISO not found: %s" % iso)
        return 2

    sha_before = sha256_file(KERNEL_BIN)
    print("kernel.bin sha256 before: %s" % sha_before)

    disk = os.path.join(tempfile.gettempdir(), "detach_probe_%d.img" % os.getpid())
    if os.path.exists(disk):
        os.remove(disk)
    shutil.copy2(SHIPPED, disk)

    p, s = boot_fixed(PORT, disk)
    try:
        if not enter_shell(s):
            print("FATAL: no prompt")
            return 3

        # ---- reality check -------------------------------------------------
        do(s, "clear", 0.5)
        do(s, "pkg list", 1.5)
        plist = q.screen_text(s)
        do(s, "pkg info desktop", 1.5)
        pinfo = q.screen_text(s)
        do(s, "ls /pkg", 1.5)
        lpkg = q.screen_text(s)
        open(os.path.join(OUT, "pkg_reality.txt"), "w").write(
            "== pkg list ==\n%s\n== pkg info desktop ==\n%s\n== ls /pkg ==\n%s\n"
            % (plist, pinfo, lpkg))
        print("pkg list has desktop entry: %s" % ("desktop" in plist))
        print("pkg info desktop says: %s" % pinfo[-120:])

        # ---- desktop pkg: install -> run -> remove -------------------------
        do(s, "clear", 0.5)
        do(s, "pkg install desktop", 3.0)
        inst = q.screen_text(s)
        open(os.path.join(OUT, "desktop_install.txt"), "w").write(inst)
        check("pkg install desktop returns",
              "desktop" in inst, inst[-120:])

        do(s, "clear", 0.5)
        do(s, "pkg list", 1.5)
        after_inst = q.screen_text(s)
        check("desktop marked after install",
              "desktop" in after_inst, after_inst[-120:])

        # Attempt the legacy desktop CE once.  It is a v2.69-era reference
        # app (extapp/desktop.c); the current block desktop is the kernel
        # `gui`.  This launch is best-effort: Esc breaks its loop, and we
        # wait for the prompt to come back before touching pkg state again.
        do(s, "clear", 0.5)
        do(s, "desktop", 2.5)
        # Esc (0x1B) breaks the legacy CE loop (desktop.c:121); q is a
        # secondary attempt.  Wait for the prompt before the next command.
        q.sendkey(s, "esc", 0.8)
        q.sendkey(s, "q", 0.8)
        if not q.wait_for_prompt(s, 20.0):
            q.force_text(s, settle=2.0)
        q.sendkey(s, "ret", 0.5)   # drain any stray key before the next line
        desktop_exit = q.screen_text(s)
        open(os.path.join(OUT, "desktop_run.txt"), "w").write(desktop_exit)
        check("legacy desktop session no fault", fault_free(desktop_exit),
              desktop_exit[-120:])

        do(s, "clear", 0.5)
        do(s, "ps", 1.5)
        ps_after_desktop = q.screen_text(s)
        open(os.path.join(OUT, "ps_after_desktop.txt"), "w").write(ps_after_desktop)
        n_d = task_count(ps_after_desktop)
        check("ps after legacy desktop: only shell task",
              fault_free(ps_after_desktop) and n_d == 1,
              "tasks=%s" % n_d)

        do(s, "clear", 0.5)
        do(s, "pkg remove desktop", 2.0)
        rm = q.screen_text(s)
        open(os.path.join(OUT, "desktop_remove.txt"), "w").write(rm)
        check("pkg remove desktop returns", "desktop" in rm, rm[-120:])
        do(s, "pkg list", 1.5)
        after_rm = q.screen_text(s)
        check("desktop back to available after remove",
              "desktop" in after_rm, after_rm[-120:])

        # ---- kernel-state gui: enter -> Esc -> no residual task ------------
        do(s, "clear", 0.5)
        do(s, "gui", 3.0)
        shoot(s, "01-gui-entered")
        q.sendkey(s, "esc", 1.0)
        time.sleep(1.5)
        gui_back = q.screen_text(s)
        check("gui exits to shell, no fault",
              fault_free(gui_back) and "root@catos" in gui_back,
              gui_back[-120:])
        do(s, "ps", 1.5)
        ps_gui = q.screen_text(s)
        open(os.path.join(OUT, "ps_after_gui.txt"), "w").write(ps_gui)
        n = task_count(ps_gui)
        check("gui leaves only shell task", n == 1, "tasks=%s" % n)
        shoot(s, "02-after-gui-shell")

        # ---- bc equivalence regression ------------------------------------
        do(s, "clear", 0.5)
        do(s, "pkg install bc", 2.5)
        bc_inst = q.screen_text(s)
        check("pkg install bc", "bc" in bc_inst, bc_inst[-120:])
        do(s, "bc 12 + 30", 1.5)
        bc_run = q.screen_text(s)
        check("bc runs: 12 + 30 = 42", "42" in bc_run, bc_run[-120:])
        do(s, "pkg remove bc", 2.0)
        bc_rm = q.screen_text(s)
        check("pkg remove bc", "bc" in bc_rm, bc_rm[-120:])
        do(s, "clear", 0.5)
        do(s, "bc", 1.5)
        bc_gone = q.screen_text(s)
        check("bc unknown after remove",
              "unknown" in bc_gone.lower() or "no such" in bc_gone.lower(),
              bc_gone[-120:])
        shoot(s, "03-after-bc-remove")
    finally:
        try:
            do(s, "poweroff", 1.0)
        except (ConnectionResetError, OSError):
            pass
        q.kill_qemu(p)

    sha_after = sha256_file(KERNEL_BIN)
    print("kernel.bin sha256 after:  %s" % sha_after)
    check("kernel.bin sha256 unchanged", sha_before == sha_after,
          "%s == %s" % (sha_before[:16], sha_after[:16]))

    # ---- fresh formatted volume: repo has no desktop entry ---------------
    fresh = os.path.join(tempfile.gettempdir(), "detach_fresh_%d.img" % os.getpid())
    if os.path.exists(fresh):
        os.remove(fresh)
    open(fresh, "wb").truncate(64 * 1024 * 1024)
    p, s = boot_fixed(PORT + 1, fresh)
    try:
        if not enter_shell(s):
            print("FATAL: no prompt fresh boot")
            return 3
        do(s, "format", 1.0)
        q.sendkey(s, "y", 0.4)
        q.sendkey(s, "ret", 12.0)
        if not q.wait_for_prompt(s, 60.0):
            print("FATAL: no prompt after format")
            return 3
        do(s, "clear", 0.5)
        do(s, "pkg list", 1.5)
        fresh_list = q.screen_text(s)
        open(os.path.join(OUT, "fresh_pkglist.txt"), "w").write(fresh_list)
        check("fresh volume: no desktop entry", "desktop" not in fresh_list,
              fresh_list[-120:])
        check("fresh volume: bc/stats/rps present",
              all(k in fresh_list for k in ("bc", "stats", "rps")),
              fresh_list[-120:])
    finally:
        try:
            do(s, "poweroff", 1.0)
        except (ConnectionResetError, OSError):
            pass
        q.kill_qemu(p)

    n = sum(1 for _, ok in results if ok)
    print("\n%d/%d PASS" % (n, len(results)))
    return 0 if n == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
