#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""v2.82 P1-4 (A1.4): /etc/cde.conf appearance mirror on real QEMU.

Checks, against a fixed raw disk copy so writes survive a power cycle:

  boot1  - record current theme; `theme light`; /etc/cde.conf must contain
           theme=light; capture pkg list / pkg info desktop / ls /pkg for the
           detach notes (desktop pkg availability).
  boot2  - after clean poweroff + reboot, `theme` still reports light and the
           cde.conf mirror survived; then `rm /etc/cde.conf`: the pure shell
           must stay clean (no crash, prompt back).
  boot3  - a freshly formatted volume has NO /etc/cde.conf: boot must fall
           back to the built-in default scheme and the shell must come up.

Usage:
  python3 tests/cdeconf_probe.py [catos.iso]
"""
import os
import re
import shutil
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qemu_driver as q

ROOT = os.path.dirname(HERE)
ISO = os.path.join(ROOT, "kernel_v25", "catos.iso")
OUT = os.path.join(ROOT, "artifacts", "v282", "p1-4")
os.makedirs(OUT, exist_ok=True)
# Raw images live on a local fs: QEMU's OFD write lock fails on the network
# mount the workspace sits on (same reason qemu_driver._stage_image exists).
import tempfile
STAGE = os.path.join(tempfile.gettempdir(), "cdeconf_probe_%d" % os.getpid())
os.makedirs(STAGE, exist_ok=True)
DISK = os.path.join(STAGE, "cdeconf_probe.img")
FRESH = os.path.join(STAGE, "fresh.img")
SHIPPED = os.path.join(ROOT, "kernel_v25", "catos_disk.img")
PORT = 4551

results = []


def check(name, ok, note=""):
    results.append((name, bool(ok)))
    print("%-4s %-34s %s" % ("PASS" if ok else "FAIL", name, note))


def boot_fixed(port, disk):
    qemu = q.find_qemu()
    if not qemu:
        raise RuntimeError("qemu-system-i386 not found on PATH")
    cmdline = [qemu, "-m", "256", "-no-reboot", "-net", "none",
               "-cdrom", ISO, "-boot", "order=d", "-display", "none",
               "-monitor", "tcp:127.0.0.1:%d,server,nowait" % port,
               "-drive", "file=%s,format=raw,if=ide,index=0,media=disk" % disk]
    proc = subprocess.Popen(cmdline, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
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


def parse_theme(text):
    """Extract (scheme, bg hex) from the `theme` no-arg output."""
    m = re.search(r"current theme:\s*(\S+)", text)
    scheme = m.group(1) if m else ""
    m2 = re.search(r"background\s+#?([0-9A-Fa-f]{6})", text)
    bg = m2.group(1) if m2 else ""
    return scheme, bg


def enter_shell(sock):
    """The shipped volume autostarts the block GUI; Esc drops back to the
    console, then force_text() switches the back-end to text for reading. """
    time.sleep(12.0)
    q.sendkey(sock, "esc", 1.0)
    time.sleep(1.0)
    q.force_text(sock)
    return q.wait_for_prompt(sock, 120.0)


def main():
    iso = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else ISO
    if not os.path.isfile(iso):
        print("ISO not found: %s" % iso)
        return 2

    # Fixed working copy; the shipped kernel_v25/catos_disk.img stays untouched.
    if os.path.exists(DISK):
        os.remove(DISK)
    shutil.copy2(SHIPPED, DISK)

    # ---- boot 1: switch to light, mirror to /etc/cde.conf -----------------
    p, s = boot_fixed(PORT, DISK)
    try:
        if not enter_shell(s):
            print("FATAL: no prompt boot1")
            return 3

        do(s, "clear", 0.5)
        do(s, "theme", 1.0)
        t0 = q.screen_text(s)
        open(os.path.join(OUT, "boot1_theme_before.txt"), "w").write(t0)
        print("boot1 theme before: %s" % t0[-160:])
        shoot(s, "01-before-theme")

        # desktop-pkg reality check (feeds the P1-6 / detach notes).
        do(s, "clear", 0.5)
        do(s, "pkg list", 1.2)
        plist = q.screen_text(s)
        do(s, "pkg info desktop", 1.2)
        pinfo = q.screen_text(s)
        do(s, "ls /pkg", 1.2)
        lpkg = q.screen_text(s)
        open(os.path.join(OUT, "boot1_pkg.txt"), "w").write(
            "== pkg list ==\n%s\n== pkg info desktop ==\n%s\n== ls /pkg ==\n%s\n"
            % (plist, pinfo, lpkg))
        print("pkg list has 'desktop': %s" % ("desktop" in plist))

        do(s, "clear", 0.5)
        do(s, "theme light", 1.2)
        do(s, "theme", 1.0)
        t1t = q.screen_text(s)
        sch1, bg1 = parse_theme(t1t)
        check("theme light applied (bg F6F6F4)", sch1 == "light" and bg1 == "F6F6F4",
              "scheme=%s bg=%s" % (sch1, bg1))
        do(s, "edit -c /etc/cde.conf", 1.2)
        t1 = q.screen_text(s)
        open(os.path.join(OUT, "boot1_cdeconf.txt"), "w").write(t1)
        check("cde.conf written with theme=light", "theme=light" in t1,
              t1[-140:])
        check("cde.conf carries live accent mirror", "accent=#8250DF" in t1,
              t1[-140:])
        shoot(s, "02-light-applied")
    finally:
        try:
            do(s, "poweroff", 1.0)
        except (ConnectionResetError, OSError):
            pass
        q.kill_qemu(p)
    time.sleep(2)

    # ---- boot 2: theme survives the reboot; then rm the mirror -------------
    p, s = boot_fixed(PORT + 1, DISK)
    try:
        if not enter_shell(s):
            print("FATAL: no prompt boot2")
            return 3

        do(s, "clear", 0.5)
        do(s, "theme", 1.2)
        t2 = q.screen_text(s)
        open(os.path.join(OUT, "boot2_theme_after.txt"), "w").write(t2)
        sch2, bg2 = parse_theme(t2)
        check("theme still light after reboot",
              sch2 == "light" and bg2 == "F6F6F4",
              "scheme=%s bg=%s" % (sch2, bg2))
        shoot(s, "03-after-reboot")

        do(s, "edit -c /etc/cde.conf", 1.2)
        t2b = q.screen_text(s)
        check("cde.conf survived reboot", "theme=light" in t2b, t2b[-140:])

        do(s, "rm /etc/cde.conf", 1.2)
        do(s, "clear", 0.5)
        do(s, "theme", 1.2)
        t2c = q.screen_text(s)
        clean = not re.search(r"page fault|kernel fault|system halted|panic",
                              t2c, re.I)
        check("shell clean after rm cde.conf", clean, t2c[-140:])
        shoot(s, "04-after-rm")
    finally:
        try:
            do(s, "poweroff", 1.0)
        except (ConnectionResetError, OSError):
            pass
        q.kill_qemu(p)
    time.sleep(2)

    # ---- boot 3: fresh formatted volume, no cde.conf -> built-in default ---
    fresh = FRESH
    if os.path.exists(fresh):
        os.remove(fresh)
    open(fresh, "wb").truncate(64 * 1024 * 1024)
    p, s = boot_fixed(PORT + 2, fresh)
    try:
        if not enter_shell(s):
            print("FATAL: no prompt boot3")
            return 3
        do(s, "format", 1.0)
        q.sendkey(s, "y", 0.4)
        q.sendkey(s, "ret", 12.0)
        if not q.wait_for_prompt(s, 60.0):
            print("FATAL: no prompt after format")
            return 3
        do(s, "clear", 0.5)
        do(s, "theme", 1.2)
        t3 = q.screen_text(s)
        open(os.path.join(OUT, "boot3_fresh_default.txt"), "w").write(t3)
        sch3, bg3 = parse_theme(t3)
        # Fresh volume: no cde.conf, hive seeded -> built-in dark default.
        check("fresh volume boots to default dark",
              sch3 == "dark" and bg3 == "0A0E14",
              "scheme=%s bg=%s" % (sch3, bg3))
        shoot(s, "05-fresh-default")
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
