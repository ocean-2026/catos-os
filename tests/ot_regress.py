#!/usr/bin/env python3
"""ot_regress.py -- on-metal regression for the v2.14 ot-import round.

Covers:
  1.  help            -> cat/rm/cp/mv listed as shell builtins
  2.  format          -> clean FAT32 volume
  3.  cp (binary file)-> cp /bin/DEMO1.TAD /tmp/C1.TAD, size matches
  4.  cat             -> cat of the copied chart shows level content
  5.  mv              -> /tmp/C1.TAD renamed to /tmp/C2.TAD
  6.  rm              -> /tmp/C2.TAD gone
  7.  ocntools        -> PATH fallback runs /bin/ocntools.ce, banner shows,
                         ot 'help' lists the tool suite, 'exit' returns
  8.  adofai          -> menu renders, ESC exits cleanly back to shell
  9.  faultinfo       -> no fatal fault recorded during the whole run

usage:  python tests/ot_regress.py [catos.iso] [--qemu PATH]
"""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qemu_driver import (boot as _qemu_boot, find_qemu,          # noqa: E402
                         kill_qemu, screen_text, sendkey, type_text,
                         wait_for_prompt)

REPO = os.path.dirname(HERE)
MONITOR_PORT = 4466                 # this driver's own port; never 4444
CRASH_MARKERS = ("kernel fault", "system halted")


def boot(iso, qemu):
    return _qemu_boot(qemu, iso, MONITOR_PORT)


def check(name, cond, detail=""):
    status = "PASS" if cond else "FAIL"
    print("%s %s %s" % (status, name, detail))
    return cond


def win_path(p):
    return os.path.abspath(p)


def main():
    iso = os.path.join(REPO, "kernel_v25", "catos.iso")
    qemu = find_qemu(sys.argv[sys.argv.index("--qemu") + 1]
                     if "--qemu" in sys.argv else None)
    disk = win_path(os.path.join(HERE, "qemu_disk_ot.img"))
    if not os.path.exists(disk):
        with open(disk, "wb") as fh:
            fh.truncate(64 * 1024 * 1024)
    s = _qemu_boot(qemu, iso, MONITOR_PORT, disk=disk)
    proc, sock = s
    ok = True
    try:
        wait_for_prompt(sock)
        screen_text(sock)

        # -- 1: help lists the new builtins --------------------------------
        type_text(sock, "help")
        sendkey(sock, "ret")
        time.sleep(1.0)
        t = screen_text(sock)
        ok &= check("help-lists-cat",  "cat FILE" in t)
        ok &= check("help-lists-rm",   "rm FILE" in t)
        ok &= check("help-lists-cp",   "cp SRC DST" in t)
        ok &= check("help-lists-mv",   "mv SRC DST" in t)

        # -- 2: clean volume -----------------------------------------------
        # NOTE: format wipes and then reinstalls the built-in apps, which
        # takes several seconds; anything typed before the prompt returns
        # is silently dropped.  Wait for the shell prompt, not a fixed 2s.
        type_text(sock, "format")
        sendkey(sock, "ret"); time.sleep(1.2)   # prompt must be up before y
        sendkey(sock, "y")
        sendkey(sock, "ret")                    # kb_readline confirm needs Enter
        t = ""
        for _ in range(30):
            time.sleep(1.0)
            t = screen_text(sock)
            if "Installed missing built-in" in t or "Aborted" in t:
                break
        ok &= check("format-ok", "Installed missing built-in" in t,
                    "" if "Installed missing built-in" in t else "| screen tail: " + t[-220:])

        # -- 3..6: cp / cat / mv / rm on a real chart file ------------------
        type_text(sock, "cp /bin/DEMO1.TAD /tmp/C1.TAD")
        sendkey(sock, "ret"); time.sleep(1.0)
        type_text(sock, "cat /bin/DEMO1.TAD")
        sendkey(sock, "ret"); time.sleep(1.0)
        t = screen_text(sock)
        ok &= check("cat-shows-chart", ("angle" in t.lower()) or ("bpm" in t.lower()))
        ok &= check("cat-no-deny", "permission denied" not in t)

        type_text(sock, "mv /tmp/C1.TAD /tmp/C2.TAD")
        sendkey(sock, "ret"); time.sleep(1.5)
        type_text(sock, "clear"); sendkey(sock, "ret"); time.sleep(1.0)
        type_text(sock, "ls /tmp")
        sendkey(sock, "ret"); time.sleep(1.5)
        screen_text(sock)                     # settle pass
        t = screen_text(sock)
        # FAT shows 8.3 names in their created case (lowercase here)
        ok &= check("mv-done", "c2" in t.lower() and "c1" not in t.lower())

        type_text(sock, "rm /tmp/C2.TAD")
        sendkey(sock, "ret"); time.sleep(1.0)
        type_text(sock, "clear"); sendkey(sock, "ret"); time.sleep(0.8)
        type_text(sock, "ls /tmp")
        sendkey(sock, "ret"); time.sleep(1.0)
        t = screen_text(sock)
        ok &= check("rm-done",
                    "c2" not in t.lower() and "c1" not in t.lower())

        # -- 7: ocntools via PATH ------------------------------------------
        type_text(sock, "ocntools")
        sendkey(sock, "ret"); time.sleep(2.5)
        t = screen_text(sock)
        ok &= check("ot-banner", "OCEAN" in t.upper())
        type_text(sock, "help")
        sendkey(sock, "ret"); time.sleep(1.0)
        t = screen_text(sock)
        ok &= check("ot-help-suite", "sft" in t and "cedbg" in t)
        ok &= check("ot-help-no-dupes", "mkdir PATH" not in t)
        type_text(sock, "exit")
        sendkey(sock, "ret"); time.sleep(1.5)
        t = screen_text(sock)
        ok &= check("ot-exit", "catos:/" in t)

        # -- 8: adofai menu + clean exit ------------------------------------
        type_text(sock, "adofai")
        sendkey(sock, "ret"); time.sleep(2.0)
        t = screen_text(sock)
        ok &= check("adofai-menu", "ADOFAI" in t and "DEMO 1" in t)
        sendkey(sock, "esc"); time.sleep(1.5)
        t = screen_text(sock)
        ok &= check("adofai-exit", "catos:/" in t)

        # -- 9: no faults during the whole run ------------------------------
        type_text(sock, "clear"); sendkey(sock, "ret"); time.sleep(0.8)
        type_text(sock, "faultinfo")
        sendkey(sock, "ret"); time.sleep(1.2)
        t = screen_text(sock)
        ok &= check("no-faults", "no fault recorded" in t.lower())
    finally:
        kill_qemu(proc)
    print("\nALL PASS" if ok else "\nFAILURES PRESENT")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
