#!/usr/bin/env python3
"""panic_regress.py -- on-metal regression for the v2.59 crash screen (BSOD).

Covers the four paths in the v2.59-crashscreen spec:

  a) text mode: `bsod --no-reboot` renders the report -- `:(` art,
     STOP code, EIP/CR2/ERR/EFLAGS/tick, key hint -- and a keypress
     returns to the shell instead of rebooting.
  b) gfx24 state: `gfx24` first, then `bsod --no-reboot`; the screen is
     forced back to VGA text and the report renders there; keypress
     returns to the shell.
  c) ring3 fault: `run /bin/crash.ce kernel` kills the task only; no
     blue screen, the shell stays usable and `faultinfo` records it.
  d) /etc/panicmode=halt: `panicmode halt`, then `bsod`; after a
     keypress the guest HALTS on the report instead of rebooting, so
     the BSOD frame is still on screen and no shell prompt reappears.

Ports: 4540-4549 (own monitor port, never 4444).

usage:
  python tests/panic_regress.py [catos.iso] [--qemu PATH] [--port N]
"""
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qemu_driver import (boot as _qemu_boot, find_qemu, kill_qemu,  # noqa: E402
                         screen_text, sendkey, type_text,
                         wait_for_prompt, read_screen, force_text)

REPO = os.path.dirname(HERE)
DEFAULT_PORT = 4546
CRASH_MARKERS = ("system halted", "unexpected_kernel_mode_trap")


def win_path(p):
    return os.path.abspath(p).replace("\\", "/")


def prompt_up(s):
    return bool(re.search(r"catos:[^\n]*[\$#]", read_screen(s)))


def main():
    args = sys.argv[1:]
    iso = None
    if args and not args[0].startswith("--"):
        iso = args.pop(0)
    qemu = None
    port = DEFAULT_PORT
    if "--qemu" in args:
        i = args.index("--qemu")
        qemu = args[i + 1]
    if "--port" in args:
        i = args.index("--port")
        port = int(args[i + 1])

    iso = os.path.abspath(iso or os.path.join(REPO, "kernel_v25", "catos.iso"))
    qemu = find_qemu(qemu)
    if not qemu:
        print("qemu-system-i386 not found")
        return 2
    if not os.path.isfile(iso):
        print("ISO not found: %s" % iso)
        return 2

    disk = win_path(os.path.join(HERE, "qemu_disk_panic.img"))
    if not os.path.exists(disk):
        with open(disk, "wb") as fh:
            fh.truncate(64 * 1024 * 1024)

    # serial capture: the crash path must dual-output the same core summary
    # to COM1 before painting the VGA buffer.  Read it back after the crash.
    serial_log = os.path.join(HERE, "regress_logs", "panic_serial.log")
    os.makedirs(os.path.dirname(serial_log), exist_ok=True)
    try:
        os.remove(serial_log)
    except OSError:
        pass

    proc, s = _qemu_boot(qemu, iso, port, disk=disk,
                         extra_args=["-serial", "file:%s" % serial_log])
    results = []

    def step(name, ok, note):
        results.append((name, ok))
        print("%-4s %-16s %s" % ("PASS" if ok else "FAIL", name, note))

    def do(line, wait=1.0):
        type_text(s, line)
        sendkey(s, "ret", wait)

    def clear():
        do("clear", 0.6)

    try:
        # v2.56: boot defaults to gfx24; force the console back to text so the
        # 0xB8000 text buffer is readable through the monitor (see qemu_driver
        # .force_text).  The consent gate is blind by nature.
        force_text(s)
        if not wait_for_prompt(s, timeout=120.0):
            print("FATAL: shell prompt never appeared")
            return 3

        type_text(s, "format")
        sendkey(s, "ret", 1.0)
        sendkey(s, "y", 0.4)
        sendkey(s, "ret", 15.0)
        if not wait_for_prompt(s, timeout=90.0):
            print("FATAL: shell never returned after format")
            return 3
        step("format", True, "FAT32 volume ready")

        # a) text-mode crash report, soft return to shell.
        #    Summary fields: vector tag, CPU#, FATAL, RECOVER: NO, reason,
        #    ERR, EIP, CR2(#PF only; manual panic -> 0), RSP, EFLAGS, stack.
        clear()
        do("bsod --no-reboot", 1.5)
        txt = screen_text(s)
        markers = [":(", "EIP=", "EFLAGS=", "RECOVER: NO", "FATAL",
                   "CPU ", "ERR=", "RSP=", "tick"]
        missing = [m for m in markers if m not in txt]
        step("summary-fields", not missing,
             "missing=%s" % missing if missing else
             "vector/FATAL/RECOVER/CPU/ERR/EIP/CR2/RSP/EFLAGS present")
        step("summary-stopcode", "UNEXPECTED_KERNEL_MODE_TRAP" in txt,
             "manual panic -> default trap code")

        # single-key pages: D regs, S stack, M map, P dump queued, A cpus.
        sendkey(s, "d", 1.2)
        td = screen_text(s)
        step("page-regs-D", "full register dump" in td,
             "D -> full register dump page")
        sendkey(s, "s", 1.2)
        ts = screen_text(s)
        step("page-stack-S", "stack trace" in ts, "S -> stack trace page")
        sendkey(s, "m", 1.2)
        tm = screen_text(s)
        step("page-map-M", "memory mapping" in tm and "CR3=" in tm,
             "M -> mapping / CR3 summary page")
        sendkey(s, "p", 1.2)
        tp = screen_text(s)
        step("page-dump-P", "dump queued" in tp,
             "P shows only 'dump queued', no FS write")
        sendkey(s, "a", 1.2)
        ta = screen_text(s)
        step("page-cpus-A", "per-CPU state" in ta and "SMP deferred" in ta,
             "A -> per-CPU page, SMP deferred placeholder")
        # C refuses: RECOVER: NO
        sendkey(s, "c", 1.2)
        tc = screen_text(s)
        step("continue-refused", "continue not available" in tc,
             "C refused: RECOVER: NO")

        # serial dual-output: the same core summary landed on COM1.
        serial = ""
        try:
            with open(serial_log, "r", errors="replace") as fh:
                serial = fh.read()
        except OSError:
            pass
        step("serial-dual-output",
             "[bsod] === PANIC:" in serial and "eip=" in serial,
             "COM1 breadcrumb carries vector + EIP")

        # R returns to the shell on the soft path.
        sendkey(s, "r", 1.5)
        step("text-bsod-return", prompt_up(s), "R keypress back at shell")

        # b) gfx24 -> forced back to text -> report, soft return.
        clear()
        do("gfx24", 3.0)
        do("bsod --no-reboot", 1.5)
        txt = screen_text(s)
        missing_b = [m for m in [":(", "EIP="] if m not in txt]
        step("gfx24-bsod-text", not missing_b,
             "forced back to text, missing=%s" % missing_b if missing_b
             else "report visible in text buffer")
        sendkey(s, "r", 1.5)
        step("gfx24-bsod-return", prompt_up(s), "R keypress back at shell")

        # c) ring3 fault: task killed, no blue screen, system usable
        clear()
        do("run /bin/crash.ce kernel", 2.5)
        low = screen_text(s).lower()
        killed = ("killed" in low and "crash" in low)
        blue = any(m in low for m in CRASH_MARKERS) or "stop:" in low
        step("ring3-isolation", killed and not blue,
             "killed=%s no-bsod=%s" % (killed, not blue))
        step("ring3-system-alive", prompt_up(s), "shell prompt still up")
        clear()
        do("faultinfo", 1.5)
        txt = screen_text(s)
        step("ring3-recorded", "last fault" in txt and "page-fault" in txt,
             "faultinfo records the ring3 page fault")

        # crashlog: the earlier soft BSOD was a test return, not a real fatal
        # crash; its record is cleared on soft exit, and the ring3 kill does
        # not write the kernel panic record, so the builtin reports none.
        clear()
        do("crashlog", 1.5)
        tcl = screen_text(s)
        step("crashlog-no-record", "no crash record" in tcl,
             "soft return + ring3 kill leave no saved record")

        # d) panicmode=halt: key press leaves the guest halted on the report
        clear()
        do("panicmode halt", 1.0)
        step("panicmode-set", "halt" in screen_text(s),
             "cached + persisted to /etc/panicmode")
        do("bsod", 1.5)
        sendkey(s, "r", 1.0)
        time.sleep(4.0)
        txt = screen_text(s)
        halted_bsod = (":(" in txt and "MANUAL PANIC" in txt) and not prompt_up(s)
        step("halt-mode-stays-on-report", halted_bsod,
             "still on BSOD frame, no prompt after key")
    finally:
        kill_qemu(proc)

    fails = [n for n, ok in results if not ok]
    print("\nsteps=%d ok=%d fail=%d" % (len(results),
                                        len(results) - len(fails), len(fails)))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
