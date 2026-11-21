#!/usr/bin/env python3
"""Capture the v2.67 TUI dashboard: `dash` on each tab, then quit.

shoot.py drives one shell command per screenshot and waits for a prompt
after each; an interactive screen never returns to the prompt, so the job
finishes by pressing ESC and capturing the restored shell instead.
"""
import os, sys, time, subprocess
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import qemu_driver as qd


def shot(s, outdir, n, name):
    p = os.path.join(outdir, "%02d-%s.ppm" % (n, name))
    qd.cmd(s, "screendump %s" % p, wait=1.5)
    print("shot", p, os.path.getsize(p) if os.path.exists(p) else "MISSING")


def main():
    iso = sys.argv[1]
    outdir = sys.argv[2]
    disk = sys.argv[3] if len(sys.argv) > 3 else None
    if disk is None:
        # v2.67.1: the previous "empty screen, no prompt" failures were the
        # guest booting without the FAT32 image and therefore without /bin -
        # it sat in the "no filesystem" path instead of the shell.  Default
        # to the disk that ships next to the ISO.
        cand = os.path.join(os.path.dirname(os.path.abspath(iso)), "catos_disk.img")
        if os.path.exists(cand):
            disk = cand
    os.makedirs(outdir, exist_ok=True)
    qemu = qd.find_qemu()
    proc, s = qd.boot(qemu, iso, 4455, disk=disk)
    try:
        # GRUB + kernel install of the built-in apps takes ~20 s; a prompt
        # probe before that reads a half-drawn screen and always fails.
        time.sleep(22.0)
        if not qd.wait_for_prompt(s, timeout=60.0):
            print("no prompt; screen:\n" + qd.read_screen(s)); return 1
        qd.force_text(s)
        qd.wait_for_prompt(s, timeout=30.0)
        qd.type_text(s, "dash")
        qd.sendkey(s, "ret", 2.5)
        time.sleep(1.0)
        shot(s, outdir, 0, "dash-system")
        qd.sendkey(s, "tab", 1.2); shot(s, outdir, 1, "dash-storage")
        qd.sendkey(s, "tab", 1.2); shot(s, outdir, 2, "dash-network")
        qd.sendkey(s, "tab", 1.2); shot(s, outdir, 3, "dash-security")
        qd.sendkey(s, "esc", 2.0)
        shot(s, outdir, 4, "back-to-shell")
        print("screen tail:\n" + qd.read_screen(s)[-400:])
        return 0
    finally:
        qd.kill_qemu(proc)


if __name__ == "__main__":
    sys.exit(main())
