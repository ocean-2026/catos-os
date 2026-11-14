#!/usr/bin/env python
"""Boot a given ISO with a given disk and dump the EARLY boot screen.

Used to check whether /etc/autoexec/* is executed at boot: the shell's
`ls /` output would scroll the boot screen away, so the marker has to be
looked for in the first screenful after the prompt appears.

    python tests/autostart_check.py <iso> <disk> <port> <label>

Prints: the first screenful, whether the marker was found, and the exit code.
"""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qemu_driver as qd

QEMU = r"C:\qemu-portable\qemu-system-i386.exe"
MARKER = "PWNED_MARKER"


def main():
    if len(sys.argv) < 5:
        print("usage: autostart_check.py <iso> <disk> <port> <label>")
        return 2
    iso, disk, port, label = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]

    proc = s = None
    try:
        proc, s = qd.boot(QEMU, os.path.abspath(iso), port, disk=os.path.abspath(disk))
        ok = qd.wait_for_prompt(s, timeout=90.0)
        # capture the FIRST screenful: the boot text is already on it
        time.sleep(1.0)
        txt = qd.screen_text(s)
        found = MARKER in txt
        banner = "CatOS 0.3" in txt or "Bootloader" in txt
        autoexec_lines = [ln for ln in txt.splitlines()
                          if "autostart:" in ln or MARKER in ln]
        print("=== [%s] iso=%s" % (label, os.path.basename(iso)))
        print("prompt=%s marker_found=%s banner_seen=%s" % (ok, found, banner))
        for ln in autoexec_lines[:10]:
            print("  boot-line: %s" % ln.strip())
        if not found:
            print("  --- first screenful (marker absent) ---")
            for ln in txt.splitlines()[:30]:
                print("  | %s" % ln.rstrip())
        return 0 if found else 1
    finally:
        if s is not None:
            try:
                s.close()
            except Exception:
                pass
        qd.kill_qemu(proc)


if __name__ == "__main__":
    sys.exit(main())
