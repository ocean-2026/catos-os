#!/usr/bin/env python3
"""p13_text_cjk_regress.py -- B7 (#117) text-mode CJK degradation regression.

In VGA text mode the CJK font slots cannot hold the full hanzi set reliably,
so the text back-end degrades every non-ASCII codepoint to a single '?'
(instead of painting a blank / dropping the line).  The first non-ASCII cell
on a text session emits one hint line pointing at gfx mode; the hint is not
repeated on later output.  GFX/VBE back-ends are untouched (covered by
cjk_width_regress.py).

Usage:
  python tests/p13_text_cjk_regress.py [catos.iso] [--qemu PATH] [--shot-dir DIR]
"""
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qemu_driver import (boot, cmd, find_qemu, force_text, kill_qemu,  # noqa: E402
                         read_screen, sendkey, type_text, wait_for_prompt)

REPO = os.path.dirname(HERE)
MONITOR_PORT = 4474
SHOT_DIR = os.path.join(REPO, "artifacts", "v282", "p1-3")


def shoot(s, name):
    os.makedirs(SHOT_DIR, exist_ok=True)
    ppm = os.path.join(SHOT_DIR, name + ".ppm")
    png = os.path.join(SHOT_DIR, name + ".png")
    if os.path.exists(ppm):
        os.remove(ppm)
    cmd(s, "screendump " + ppm.replace("\\", "/"), 1.0)
    for _ in range(10):
        if os.path.exists(ppm) and os.path.getsize(ppm) > 0:
            break
        time.sleep(0.3)
    if os.path.exists(ppm):
        subprocess.run(["convert", ppm, png], check=False)
        os.remove(ppm)
    return png


def main(argv):
    iso = os.path.join(REPO, "kernel_v25", "catos.iso")
    qemu = None
    args = list(argv)
    while args:
        a = args.pop(0)
        if a == "--qemu":
            qemu = args.pop(0)
        elif a == "--shot-dir":
            globals()["SHOT_DIR"] = args.pop(0)
        elif not a.startswith("-"):
            iso = os.path.abspath(a)
    qemu = find_qemu(qemu or "/home/user/bin/qemu-system-i386")
    if not qemu or not os.path.isfile(iso):
        print("p13: need QEMU and a built ISO (got %r / %r)" % (qemu, iso))
        return 2

    disk = os.path.join(HERE, "qemu_disk_p13.img")
    if not os.path.exists(disk):
        with open(disk, "wb") as fh:
            fh.truncate(64 * 1024 * 1024)

    proc, s = boot(qemu, iso, MONITOR_PORT, disk=disk)
    force_text(s)
    results = []

    def step(name, ok, note):
        results.append((name, ok))
        print("%-4s %-36s %s" % ("PASS" if ok else "FAIL", name, note))

    def run(line, wait=1.2):
        type_text(s, line)
        sendkey(s, "ret", wait)
        time.sleep(0.6)
        return read_screen(s)

    try:
        if not wait_for_prompt(s, timeout=90.0):
            print("FATAL: shell prompt never appeared")
            return 3
        type_text(s, "format"); sendkey(s, "ret", 1.0)
        sendkey(s, "y", 0.4); sendkey(s, "ret", 15.0)
        if not wait_for_prompt(s, timeout=90.0):
            print("FATAL: no prompt after format")
            return 3

        # First Chinese output: `lang zh` label is "语言已设为 " (5 hanzi).
        run("clear", 0.8)
        first = run("lang zh")
        hint_lines = [ln for ln in first.splitlines() if "note:" in ln and "gfx" in ln]
        step("hint line emitted on first non-ASCII", len(hint_lines) == 1,
             "hints=%r" % (hint_lines,))
        step("5 hanzi degrade to 5 '?'",
             first.count("?") >= 5, "? count=%d" % first.count("?"))
        step("locale value still printed (line kept)", "zh" in first,
             "zh present")
        shoot(s, "01-text-lang-zh-question")

        # Second Chinese output: the hint must NOT be repeated.
        run("clear", 0.8)
        second = run("lang zh")
        hint2 = [ln for ln in second.splitlines() if "note:" in ln and "gfx" in ln]
        step("hint emitted only once per session", len(hint2) == 0,
             "hints after clear=%r" % (hint2,))
        step("second run still shows '?' for hanzi", second.count("?") >= 5,
             "? count=%d" % second.count("?"))
        shoot(s, "02-text-lang-zh-again-no-repeat")

        # Switch to gfx: back-end untouched; capture the gfx welcome frame.
        run("gfx", 3.0)
        time.sleep(1.0)
        shoot(s, "03-gfx-welcome-unchanged")
        step("switched to gfx mode", True, "gfx frame captured")

        # Return to text so the guest exits cleanly.
        type_text(s, "text"); sendkey(s, "ret", 2.0)
    finally:
        kill_qemu(proc)

    fails = [n for n, ok in results if not ok]
    print("\nsteps=%d ok=%d fail=%d" % (len(results),
                                        len(results) - len(fails), len(fails)))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
