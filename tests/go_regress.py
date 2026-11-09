#!/usr/bin/env python3
"""go_regress.py -- on-metal acceptance for the Go subset interpreter and
the node interpreter (both now live in the kernel image as /bin/go.ce and
/bin/node.ce).

Both apps print their results on the VGA text console.  Rather than
OCR-ing screenshots, this driver reads the text-mode frame buffer
directly through the QEMU monitor's physical-memory dump ("xp"), which is
what the original project's go_qemu_test.py did.  0xB8000 is the VGA
text buffer; every other byte is the attribute byte, so the reader
skips it and keeps the character bytes only.

Steps:
  0. boot the ISO and format the disk (so /bin has the built-ins)
  1. ls /bin           -- go.ce and node.ce must be installed
  2. run /bin/go.ce    -- built-in demo: "fib(10)= 55", "demo done",
                          "go: exit ok"
  3. run /bin/go.ce /BIN/TEST.GO
                       -- the self-checking corpus: "ALL PASS"
  4. run /bin/go.ce /BIN/SYN.GO
                       -- a deliberate syntax error must be reported
  5. run /bin/node.ce /BIN/TEST.JS
                       -- the node interpreter's own test script

usage:
  python tests/go_regress.py <catos.iso> [--qemu PATH] [--keep]
"""
import os
import re
import socket
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
# The QEMU lifecycle lives in tests/qemu_driver.py: PID-safe teardown,
# the VGA text-buffer decoder and the 'clear' (not 'cls') convention.
from qemu_driver import (boot as _qemu_boot, cmd, find_qemu,  # noqa: E402
                          kill_qemu, read_screen, screen_text,
                          sendkey, type_text)

HERE = os.path.dirname(os.path.abspath(__file__))
MONITOR = ("127.0.0.1", 4444)
VGA_TEXT = 0xB8000
VGA_COLS = 80
VGA_ROWS = 25

DEFAULT_QEMU_CANDIDATES = [
    r"C:\qemu-portable\qemu-system-i386.exe",
    r"C:\Program Files\qemu\qemu-system-i386.exe",
    r"C:\Program Files\QEMU\qemu-system-i386.exe",
    "qemu-system-i386",
]

# The script corpus the acceptance steps run against.  The interpreters
# read their source from the FAT volume, so these have to be injected
# into /BIN before the interpreter steps run.
REPO = os.path.dirname(HERE)
CASE_FILES = [
    (os.path.join(REPO, "kernel_v25", "extapp", "cases", "TEST.GO"), "TEST.GO"),
    (os.path.join(REPO, "kernel_v25", "extapp", "cases", "SYN.GO"), "SYN.GO"),
    (os.path.join(REPO, "kernel_v25", "extapp", "cases", "DEMO.GO"), "DEMO.GO"),
    (os.path.join(REPO, "kernel_v25", "node", "TEST.JS"), "TEST.JS"),
]


def wait_for_prompt(s, timeout=90.0, poll=2.0):
    """Wait until the CatOS shell prompt is on screen.

    A fixed sleep is not enough here: the kernel boots through GRUB and
    then installs the built-in apps, and a command typed before the
    shell is listening is simply dropped -- which is what made the
    `format` step silently do nothing and left the disk image blank.
    Polling the real VGA frame buffer for the prompt is the reliable
    signal that the shell is ready for input.

    The match has to be anchored on the prompt's actual shape
    ("catos" ":" <pwd> "$"), not on the bare word "catos": the GRUB
    boot menu also contains "catos" (it names the CatOS entry), and a
    loose match returns immediately while the kernel is still booting.
    """
    pattern = re.compile(r"catos:[^\n]*\$")
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            if pattern.search(read_screen(s)):
                return True
        except Exception:
            pass                      # monitor hiccup: keep polling
        time.sleep(poll)
    return False


def main():
    args = sys.argv[1:]
    iso = None
    if args and not args[0].startswith("--"):
        iso = args.pop(0)
    qemu = None
    if "--qemu" in args:
        i = args.index("--qemu")
        if i + 1 < len(args):
            qemu = args[i + 1]
    if iso is None:
        iso = os.path.join(os.path.dirname(HERE), "kernel_v25", "catos.iso")
    iso = os.path.abspath(iso)
    qemu = find_qemu(qemu)
    if not qemu:
        print("qemu-system-i386 not found")
        return 2

    disk = os.path.join(HERE, "qemu_disk_go.img")
    if not os.path.exists(disk):
        with open(disk, "wb") as fh:
            fh.truncate(64 * 1024 * 1024)

    # ---- pass 1: format the volume ----
    # The FAT volume has to exist before anything can be injected into
    # it, and the kernel's `format` command is the only thing that
    # creates it.  So the run is split in two: format now (with the
    # guest up), then stop the guest, inject the script corpus into the
    # resulting FAT image with inject_any.py, then boot again and run
    # the acceptance steps against the populated volume.
    proc, s = _qemu_boot(qemu, iso, MONITOR[1], disk=disk)
    print("monitor connected (pass 1: format)")
    if not wait_for_prompt(s):
        print("FATAL: shell prompt never appeared in pass 1")
        kill_qemu(proc)
        return 3
    type_text(s, "format")
    sendkey(s, "ret", 1.0)
    sendkey(s, "y", 0.4)
    sendkey(s, "ret", 12.0)            # fs_format(64 MiB) + install
    kill_qemu(proc)
    time.sleep(3)

    # ---- inject the script corpus into /BIN ----
    for src, name83 in CASE_FILES:
        if not os.path.isfile(src):
            print("inject: SKIP %s (missing %s)" % (name83, src))
            continue
        rc = subprocess.run([sys.executable,
                             os.path.join(HERE, "inject_any.py"),
                             disk, src, name83],
                            capture_output=True, text=True)
        if rc.returncode != 0:
            print("inject: FAIL %s -- %s %s"
                  % (name83, rc.stdout.strip(), rc.stderr.strip()))
        else:
            print("inject: OK   %s" % name83)

    # ---- pass 2: boot with the corpus in place ----
    proc, s = _qemu_boot(qemu, iso, MONITOR[1], disk=disk)
    print("monitor connected (pass 2: acceptance)")
    if not wait_for_prompt(s):
        print("FATAL: shell prompt never appeared in pass 2")
        kill_qemu(proc)
        return 3

    results = []

    def step(name, fn):
        try:
            ok, note = fn()
            results.append((name, ok, note))
            print("%-4s %-20s %s" % ("PASS" if ok else "FAIL", name, note))
        except Exception as e:
            results.append((name, False, str(e)))
            print("%-4s %-20s %s" % ("ERR ", name, e))

    def do(line, wait=1.5):
        type_text(s, line)
        sendkey(s, "ret", wait)

    def clear():
        """Blank the screen so the next command's output is unambiguous.

        The kernel's command is `clear` -- `cls` is not in the shell's
        dispatch table, so typing `cls` just printed "unknown command"
        and left the previous output on screen, which quietly made the
        substring assertions match against stale text.
        """
        do("clear", 1.0)

    def step_format():
        return True, "formatted (done before injection)"

    def step_ls_bin():
        clear()
        do("ls /bin", 2.0)
        txt = screen_text(s)
        ok = "go.ce" in txt and "node.ce" in txt
        return ok, "go.ce=%s node.ce=%s" % ("go.ce" in txt, "node.ce" in txt)

    def run_go(argv_line, expect, label, wait=6.0):
        clear()
        do(argv_line, wait)
        txt = screen_text(s)
        missing = [e for e in expect if e not in txt]
        if missing:
            # Print the screen so a failure is diagnosable from the log
            # alone -- the assertion alone does not say whether the app
            # failed, printed a different message, or never ran at all.
            print("---- screen after %r ----" % argv_line)
            print(read_screen(s))
            print("---------------------------")
        return (not missing), ("missing=%r" % missing if missing else "all found")

    def step_go_demo():
        return run_go("run /bin/go.ce",
                      ["fib(10)= 55", "demo done", "go: exit ok"],
                      "demo")

    def step_go_test():
        return run_go("run /bin/go.ce /BIN/TEST.GO",
                      ["ALL PASS", "go: exit ok"],
                      "test", wait=12.0)

    def step_go_syn():
        return run_go("run /bin/go.ce /BIN/SYN.GO",
                      ["syntax error at line 3"],
                      "syn", wait=6.0)

    def step_node_test():
        return run_go("run /bin/node.ce /BIN/TEST.JS",
                      ["ALL PASS", "node: exit ok"],
                      "node", wait=12.0)

    def step_cat_bin():
        """Dump /bin so a missing script file is distinguishable from a
        broken interpreter.

        There is no `cat` command in the CatOS shell, so the listing is
        the only way to prove the injected corpus is actually on the
        volume -- asking for `cat` just produced "unknown command".
        """
        clear()
        do("ls /bin", 3.0)
        listing = screen_text(s)
        print("---- ls /bin ----")
        print(read_screen(s))
        print("-----------------")
        low = listing.lower()
        missing = [f for f in ("test.go", "syn.go", "test.js")
                   if f not in low]
        return (not missing), ("missing=%r" % missing if missing else "all present")

    step("format", step_format)
    step("ls-bin", step_ls_bin)
    step("cat-bin", step_cat_bin)
    step("go-demo", step_go_demo)
    step("go-test", step_go_test)
    step("go-syntax-err", step_go_syn)
    step("node-test", step_node_test)

    if "--keep" not in args:
        kill_qemu(proc)
    fails = [r for r in results if not r[1]]
    print("\nsteps=%d ok=%d fail=%d" % (len(results),
                                        len(results) - len(fails), len(fails)))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
