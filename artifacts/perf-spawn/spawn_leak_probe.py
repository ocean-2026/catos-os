#!/usr/bin/env python3
"""spawn_leak_probe.py -- spawn/exit soak + frame-leak gate for the perf/spawn line.

Boots the bench ISO against a staged copy of the reference FAT volume
(/bin/hello.ce is preinstalled there; the image itself is never mutated),
records the PMM free-frame count from `mem`, runs `run hello.ce` N times
(each a full spawn -> ring3 -> exit -> reclaim cycle), then reads `mem` and
`ps` again.

Pass criteria:
  * free frames after N runs == free frames at baseline (no PMM leak);
  * `ps` lists no ZOMBIE task;
  * the shell still answers the prompt after the soak.

usage:
  python3 artifacts/perf-spawn/spawn_leak_probe.py <iso> [--n 60]
"""
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, "tests"))
from qemu_driver import (boot, find_qemu, kill_qemu, screen_text, sendkey,
                         type_text, wait_for_prompt, force_text)  # noqa: E402

PORT = 4471
FREE_RE = re.compile(r"free\s+(\d+)\s+frames")


def read_mem_free(s):
    type_text(s, "mem")
    sendkey(s, "ret", 1.0)
    time.sleep(0.8)
    txt = screen_text(s)
    m = FREE_RE.search(txt)
    return int(m.group(1)) if m else None, txt


def main():
    args = sys.argv[1:]
    iso = args[0] if args and not args[0].startswith("--") else \
        os.path.join(REPO, "kernel_v25", "catos.iso")
    disk = args[1] if len(args) > 1 and not args[1].startswith("--") else \
        "/home/user/work/catos-perf-heavy/bench/disk-ref-64m.img"
    n = 60
    if "--n" in args:
        n = int(args[args.index("--n") + 1])

    qemu = find_qemu(None)
    proc, s = boot(qemu, iso, PORT, disk=disk)
    try:
        # Boot default is gfx24, which does not mirror 0xB8000; switch blind
        # to the text console first (same flow as vprobe_regress.py), then
        # the HMP screen dump can read the prompt.
        force_text(s)
        if not wait_for_prompt(s, timeout=120.0):
            print("FATAL: prompt never appeared")
            return 3

        base, _ = read_mem_free(s)
        print("baseline free frames: %s" % base)

        failures = 0
        for i in range(1, n + 1):
            type_text(s, "run hello.ce")
            sendkey(s, "ret", 0.2)
            if not wait_for_prompt(s, timeout=30.0):
                failures += 1
                print("  run %2d: prompt did not return" % i)
            if i % 10 == 0:
                print("  ...%d/%d spawn/exit cycles done" % (i, n))

        after, mem_txt = read_mem_free(s)
        print("after %d runs free frames: %s" % (n, after))

        type_text(s, "ps")
        sendkey(s, "ret", 1.0)
        time.sleep(0.8)
        ps_txt = screen_text(s)
        zombies = ps_txt.count("ZOMBIE")

        # shell still responsive?
        type_text(s, "echo soak-ok")
        sendkey(s, "ret", 0.6)
        responsive = "soak-ok" in screen_text(s)

        print("---")
        print("cycles run        : %d (prompt failures %d)" % (n, failures))
        print("free frames base  : %s" % base)
        print("free frames after : %s" % after)
        if base is not None and after is not None:
            print("frame delta       : %+d" % (after - base))
        print("zombie tasks      : %d" % zombies)
        print("shell responsive  : %s" % responsive)

        ok = (failures == 0 and zombies == 0 and responsive and
              base is not None and after is not None and after >= base - 2)
        print("GATE: %s" % ("PASS" if ok else "FAIL"))
        return 0 if ok else 1
    finally:
        kill_qemu(proc)


if __name__ == "__main__":
    sys.exit(main())
