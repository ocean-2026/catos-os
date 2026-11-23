#!/usr/bin/env python3
"""ime_autotoggle_probe.py -- "the IME bar comes back after every command".

Root cause under test: ime.c deactivated the IME with con_status_bar_off(),
which restores the two borrowed rows but leaves bar_active = 1.  The next
screen scroll (any command that prints a line) calls
con_status_bar_reapply() and pins the STALE candidate row back to the
bottom -- the user sees the pinyin IME pop up again right after the
command finishes.

Scenarios (text-mode guest, screen read through the VGA text plane):
  A. F2 on -> F2 off   -> run `help` (scrolls) -> bottom rows must be clean
  B. F2 on -> Esc      -> run `help` (scrolls) -> bottom rows must be clean
  C. control: F2 on    -> bottom row must show "[IME]" (the bar still works)

usage: python3 tests/ime_autotoggle_probe.py [catos.iso]
exit code 0 = all PASS
"""
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, "/home/user/work/catos-os/tests")
from qemu_driver import (boot, find_qemu, force_text, kill_qemu,  # noqa
                         read_screen, sendkey, type_text)

REPO = os.path.dirname(HERE)
ISO = os.path.join(REPO, "kernel_v25", "catos.iso")
PORT = 4595


def rows(s):
    return read_screen(s).split("\n")


def bottom(s, n=3):
    return "\n".join(rows(s)[-n:])


def has_bar(s):
    """The STAGE-1 bar paints rows-2 as a full '-' rule and rows-1 as the
    candidate list; either marker means the two-row IME area is on screen."""
    blob = bottom(s, 4)
    return ("[IME]" in blob or re.search(r"1/\d+\s+1\.", blob) is not None
            or re.search(r"^1\. .{2,}", blob, re.M) is not None)


def main(argv):
    iso = argv[1] if len(argv) > 1 else ISO
    qemu = find_qemu()
    proc, s = boot(qemu, iso, PORT)
    fails = []

    def check(name, cond, detail):
        print("  %-4s %-40s %s" % ("PASS" if cond else "FAIL", name,
                                   detail.replace("\n", " / ")[:140]))
        if not cond:
            fails.append(name)

    try:
        force_text(s)

        # C: bar shows while the IME is really on (control)
        sendkey(s, "f2", 0.6)
        check("C IME on -> bar visible", has_bar(s), bottom(s, 2))
        sendkey(s, "f2", 0.6)
        check("C IME off -> bar gone", not has_bar(s), bottom(s, 2))

        # A: toggle off by F2, then a scrolling command
        sendkey(s, "f2", 0.6)          # on
        sendkey(s, "h", 0.1)
        sendkey(s, "a", 0.1)
        sendkey(s, "o", 0.3)           # composition "hao"
        sendkey(s, "f2", 0.6)          # off  (old bug: bar_active stays 1)
        type_text(s, "help\n", 0.15)
        time.sleep(2.0)
        check("A F2-off + scrolling command", not has_bar(s), bottom(s, 2))

        # B: toggle off by Esc, then a scrolling command
        sendkey(s, "f2", 0.6)
        sendkey(s, "n", 0.1)
        sendkey(s, "i", 0.3)
        sendkey(s, "esc", 0.6)
        type_text(s, "help\n", 0.15)
        time.sleep(2.0)
        check("B Esc-off + scrolling command", not has_bar(s), bottom(s, 2))

        type_text(s, "clear\n", 0.15)
        time.sleep(1.0)
    finally:
        kill_qemu(proc)

    print("\n%s" % ("ALL PASS" if not fails else "FAIL: " + ", ".join(fails)))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
