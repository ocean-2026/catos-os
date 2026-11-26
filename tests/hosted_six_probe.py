"""Live QEMU probe: hosted 六探针 (threads/preempt/heaptest/dltest/mmaptest/locale).

Runs against a freshly built main ISO with ABI slots 80-98 (hosted, +3
after cap2).  Boots, switches from gfx to text, formats a fresh disk to
populate /bin with the built-in CE apps, then runs each of the six probes
and asserts its PASS marker on the VGA text screen.
"""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qemu_driver import (find_qemu, boot, kill_qemu, cmd, sendkey,
                         type_text, wait_for_prompt, screen_text)

ROOT = os.path.dirname(HERE)
ISO = os.path.join(ROOT, "kernel_v25", "catos.iso")
SHOTS = os.path.join(ROOT, "artifacts", "hosted-six-probe")
PORT = 4530
os.makedirs(SHOTS, exist_ok=True)

disk = os.path.join(SHOTS, "disk.img")
if not os.path.exists(disk):
    open(disk, "wb").truncate(64 * 1024 * 1024)

# (command, PASS substring, extra required substrings)
PROBES = [
    ("threads",   "THREAD PASS",   []),
    ("preempt",   "PREEMPT PASS",  []),
    ("heaptest",  "HEAPTEST PASS", []),
    ("dltest",    "dltest: PASS",  ["loaded libmath.so"]),
    ("mmaptest",  "MMAPTEST PASS", []),
    ("locale",    "LOCALE PASS",   []),
]

results = []
p, s = boot(find_qemu(), ISO, PORT, disk=disk,
            log=os.path.join(SHOTS, "qemu.log"))
try:
    time.sleep(9)
    type_text(s, "text"); sendkey(s, "ret"); time.sleep(0.6)
    sendkey(s, "y"); sendkey(s, "ret"); time.sleep(1.5)
    if not wait_for_prompt(s, 40):
        raise RuntimeError("no shell prompt after text switch")

    type_text(s, "format"); sendkey(s, "ret"); time.sleep(0.4)
    sendkey(s, "y"); sendkey(s, "ret"); time.sleep(6)

    for name, marker, extras in PROBES:
        type_text(s, "clear"); sendkey(s, "ret"); time.sleep(0.3)
        type_text(s, name); sendkey(s, "ret"); time.sleep(2.5)
        out = screen_text(s)
        ok = marker in out and all(e in out for e in extras)
        results.append((name, ok, marker))
        print("=== %s === %s" % (name, "PASS" if ok else "FAIL"))
        # dump tail for diagnosis; keep it short
        tail = out[-1600:]
        open(os.path.join(SHOTS, name + ".txt"), "w").write(out)
        print(tail)
        print()

    n = sum(1 for _, ok, _ in results if ok)
    print("SUMMARY %d/%d PASS" % (n, len(results)))
    for name, ok, marker in results:
        print("  %-10s %s (expect %r)" % (name, "PASS" if ok else "FAIL", marker))
    if n != len(results):
        raise SystemExit(1)
finally:
    kill_qemu(p)
