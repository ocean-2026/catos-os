#!/usr/bin/env python3
"""manual on-metal probe: feature surface + two candidate bug repros."""
import os, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
from qemu_driver import (boot, find_qemu, kill_qemu, screen_text, sendkey,
                         type_text, wait_for_prompt, cmd)

PORT = 4574
ISO = os.path.abspath(os.path.join(HERE, "..", "..", "kernel_v25", "catos.iso"))
DISK = os.path.abspath(os.path.join(HERE, "..", "..", "kernel_v25", "catos_disk.img"))

def run(s, c, wait=1.4):
    type_text(s, c)
    sendkey(s, "ret", wait)
    return screen_text(s)

def main():
    qemu = find_qemu(None)
    proc, s = boot(qemu, ISO, PORT, disk=DISK, extra_args=["-usb"])
    try:
        assert wait_for_prompt(s, 90), "prompt never appeared"
        print("[ok] boot -> shell prompt")

        # --- FS CRUD as root ---
        run(s, "clear")
        run(s, "mkdir /tmp/probe1")
        run(s, "touch /tmp/probe1/a.txt")
        t = run(s, "ls /tmp/probe1")
        print("[fs] ls /tmp/probe1:", "a.txt" in t)

        # --- alias overwrite repro (#120 candidate) ---
        run(s, "alias ll=ls")
        run(s, "alias ll=dir")
        t = run(s, "alias ll")
        print("[alias] after overwrite ->", t.strip())
        bug_alias = ("lsdir" in t) or ("ls" in t and "dir" in t and "=dir" not in t)

        # --- touch permission hole repro (#121 candidate) ---
        run(s, "login user")
        t = run(s, "touch /tmp/probe1/ok.txt")
        print("[perm] user touch scratch:", "denied" not in t)
        t = run(s, "touch /tmp/probe1/evil.txt")
        t2 = run(s, "ls /tmp/probe1")
        print("[perm] user touch /tmp/probe1/evil.txt listed:", "evil.txt" in t2)
        t = run(s, "touch /etc/evilprobe")
        t3 = run(s, "ls /etc")
        print("[perm] user touch /etc/evilprobe listed:", "evilprobe" in t3)
        t = run(s, "mkdir /etc/evilprobe")
        print("[perm] user mkdir /etc/evilprobe denied:", "denied" in t)
        run(s, "login root")

        # --- gfx / gfx24 / size / text round trip ---
        run(s, "gfx")
        time.sleep(1.0)
        run(s, "text")
        sendkey(s, "y", 1.2)
        t = screen_text(s)
        print("[gfx] back to text:", "text" in t.lower() or "console" in t.lower())

        run(s, "gfx24")
        time.sleep(1.0)
        run(s, "size compact")
        time.sleep(0.8)
        run(s, "size normal")
        time.sleep(0.8)
        run(s, "size large")
        time.sleep(0.8)
        run(s, "text")
        sendkey(s, "y", 1.2)
        t = screen_text(s)
        print("[gfx24+size] back to text ok:", "text" in t.lower())

        # --- IME F2 toggle (no crash) ---
        run(s, "clear")
        sendkey(s, "f2", 0.6)
        type_text(s, "ni")
        time.sleep(0.6)
        sendkey(s, "f2", 0.6)
        t = run(s, "faultinfo")
        print("[ime] no fatal fault after F2:", "panic" not in t.lower() and "halt" not in t.lower())

        # --- history / df / ps1 ---
        run(s, "history")
        t = screen_text(s)
        print("[history] entries shown:", len(t) > 0)
        t = run(s, "df")
        print("[df] output:", "Filesystem" in t)

        # --- USB hot plug: add usb-kbd via monitor, then scan ---
        r = cmd(s, "device_add usb-kbd", 1.0)
        print("[usb] device_add resp:", r.strip()[:80])
        time.sleep(1.5)
        t = run(s, "usb scan")
        print("[usb] scan:", t.strip()[-60:])
        t = run(s, "usb")
        print("[usb] status:", t.strip()[-60:])

        t = run(s, "faultinfo")
        print("[final] no fatal fault:", "panic" not in t.lower() and "halt" not in t.lower())
        print("ALIAS_BUG_CONFIRMED" if bug_alias else "ALIAS_BUG_NOT_REPRODUCED")
    finally:
        kill_qemu(proc)

if __name__ == "__main__":
    main()
