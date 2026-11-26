#!/usr/bin/env python3
"""P1-3 repro: text-mode Chinese rendering baseline observation."""
import os, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qemu_driver import (boot, find_qemu, force_text, kill_qemu,
                         read_screen, sendkey, type_text, wait_for_prompt)

REPO = os.path.dirname(HERE)
MONITOR_PORT = 4473

def main():
    iso = os.path.join(REPO, "kernel_v25", "catos.iso")
    qemu = find_qemu("/home/user/bin/qemu-system-i386")
    disk = os.path.join(HERE, "qemu_disk_p13.img")
    if not os.path.exists(disk):
        with open(disk, "wb") as fh:
            fh.truncate(64 * 1024 * 1024)
    proc, s = boot(qemu, iso, MONITOR_PORT, disk=disk)
    force_text(s)
    try:
        if not wait_for_prompt(s, timeout=90.0):
            print("FATAL: no prompt"); return 3
        type_text(s, "format"); sendkey(s, "ret", 1.0)
        sendkey(s, "y", 0.4); sendkey(s, "ret", 15.0)
        if not wait_for_prompt(s, timeout=90.0):
            print("FATAL: no prompt after format"); return 3

        def run(line, wait=1.2):
            type_text(s, line); sendkey(s, "ret", wait); time.sleep(0.6)
            return read_screen(s)

        print("===== text: lang zh =====")
        print(run("lang zh"))
        print("===== text: ls /no_such_dir (zh error) =====")
        print(run("ls /no_such_dir"))
        print("===== gfx: gfx welcome =====")
        run("gfx", 2.5)
        time.sleep(1.0)
        print(read_screen(s))
    finally:
        kill_qemu(proc)

if __name__ == "__main__":
    sys.exit(main())
