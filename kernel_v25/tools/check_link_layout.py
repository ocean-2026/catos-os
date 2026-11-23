#!/usr/bin/env python3
"""check_link_layout.py -- build-time layout guardrail for the user arena.

Background (#D11 / E3 conc regression): the kernel image is identity-mapped
and its static .bss grows upward from the link base.  The two ring3 arena
PDEs (PDE 8: 0x2000000..0x2400000, PDE 9: 0x2400000..0x2800000) are
replaced in every task directory by a private 4 KiB page table.  A kernel
static object that physically lands inside that window is therefore NOT
present under a task's CR3: a syscall running on the caller's CR3 that
dereferences such a table (acl_tab, users, tasks, pipes, ...) walks an absent
private PTE and #PFs in ring0.

This checker reads the linked PE's symbols and:

  * FATAL (exit 1): a "key" syscall-path table's interval intersects the
    arena window.  These are dereferenced on the spawn / permission /
    pipe path while still on the caller's CR3, so they must live below
    USER_IMAGE_BASE.
  * WARN: any other BSS symbol intersecting the window.  Such objects are
    normally touched only on the kernel directory; list them so a future
    change can audit whether one of them joins a syscall-path table.

Usage:
    python3 check_link_layout.py <kernel.pe> [--strict] [--strict-warn]

Default (local build): print `[link]` risk/warn lines but exit 0.
`--strict` fails on key tables in the window; `--strict-warn` also fails on
any other static symbol there.  CI / regression runs in strict mode.
"""
import os
import subprocess
import sys

# Keep in lockstep with include/paging.h.
ARENA_LO = 0x2000000
ARENA_HI = 0x2800000

# Tables dereferenced on the syscall / spawn / permission path while the
# kernel still runs on the caller task's CR3.  Their whole interval must be
# below the arena window.
KEY_TABLES = (
    "acl_tab",
    "users",
    "tasks",
    "pipes",
    "cur_task",
)


def parse_symbols(pe):
    nm = os.environ.get("NM", "nm")
    out = subprocess.run([nm, "-S", "--size-sort", pe],
                         capture_output=True, text=True, check=True).stdout
    syms = []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) < 3:
            continue
        try:
            addr = int(parts[0], 16)
        except ValueError:
            continue
        size = 0
        if len(parts) >= 4:
            try:
                size = int(parts[1], 16)
            except ValueError:
                size = 0
            name = parts[3]
            typ = parts[2]
        else:
            name = parts[2]
            typ = "?"
        syms.append((addr, size, typ, name.lstrip("_")))
    return syms


def intersects(addr, size, lo, hi):
    return addr < hi and (addr + size) > lo


def main():
    pe = sys.argv[1]
    strict = "--strict" in sys.argv[2:]
    strict_warn = "--strict-warn" in sys.argv[2:]
    syms = parse_symbols(pe)

    bss_end = max((a + s for a, s, t, n in syms if t in ("b", "B")), default=0)
    print("[link] _bss_end = 0x%08x  arena window = [0x%08x, 0x%08x)"
          % (bss_end, ARENA_LO, ARENA_HI))

    fatal = []
    warns = []
    for addr, size, typ, name in syms:
        if typ not in ("b", "B"):
            continue
        if not intersects(addr, size, ARENA_LO, ARENA_HI):
            continue
        rec = (name, addr, size, addr + size)
        if name in KEY_TABLES:
            fatal.append(rec)
        else:
            warns.append(rec)

    for name, addr, size, end in fatal:
        print("[link][RISK] key table `%s` 0x%08x..0x%08x (size 0x%x) "
              "lands in the user arena window" % (name, addr, end, size))
    for name, addr, size, end in warns:
        print("[link][warn] static `%s` 0x%08x..0x%08x sits in the arena "
              "window; verify it is never dereferenced on a caller CR3"
              % (name, addr, end))

    rc = 0
    if fatal:
        msg = "[link] %d key table(s) cross the user arena window." % len(fatal)
        if strict:
            print("[link] FAIL (--strict): " + msg)
            rc = 1
        else:
            print("[link] " + msg + " (non-fatal under B' supervisor mirror; "
                  "kept as a layout risk)")
    if warns and strict_warn:
        print("[link] FAIL (strict): %d static symbol(s) in window." % len(warns))
        rc = 1
    if rc == 0 and not fatal and not warns:
        print("[link] layout OK: syscall-path tables are below the arena window.")
    return rc


if __name__ == "__main__":
    sys.exit(main())
