#!/usr/bin/env python3
"""gfx-mode scheduler/keypress diagnostic (task #6, v2.74 -- RESOLVED).

Boots with -serial file: to capture kernel COM1 breadcrumbs.  This was
used to DISPROVE the "gfx mode stalls on task_sleep_ticks" theory:
with temporary [sleep]/[wake]/[idle]/[key]/[sc] markers in task.c (NOW
REMOVED), the log showed 200+ healthy sleep/wake cycles and a correct
[key k=0x71] for 'q'.  Kept as a reusable serial-capture harness and a
record of the method; the stock kernel no longer emits those markers.

To re-run the investigation, temporarily re-add ser_write breadcrumbs to
task_sleep_ticks/task_tick_wake/idle_main/isr_syscall, rebuild, run this
probe, then revert.  See docs/CDE-模块化设计.md §10.5.
"""
import os, sys, subprocess, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qemu_driver import (boot, find_qemu, kill_qemu, screen_text, sendkey,
                         type_text, wait_for_prompt, force_text, cmd)

KERNEL = os.path.join(os.path.dirname(HERE), "kernel_v25")
CE  = os.path.join(KERNEL, "extapp", "bin", "gfxdesk.ce")
ISO = os.path.join(KERNEL, "catos.iso")
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 4591
SERIAL = os.path.join(HERE, "dumps", "gfxsleep_serial.log")
PY = sys.executable
disk = os.path.join(HERE, "qemu_disk_gfxsleep.img")

os.makedirs(os.path.dirname(SERIAL), exist_ok=True)
subprocess.run([PY, os.path.join(KERNEL, "build", "mkdisk.py"), "-o", disk, "-m", "64"],
               check=True, capture_output=True)
subprocess.run([PY, os.path.join(HERE, "inject_any.py"), disk, CE, "GFXDESK.CE"],
               check=True, capture_output=True)
if os.path.isfile(SERIAL):
    os.remove(SERIAL)

qemu = find_qemu(None)
p, s = boot(qemu, ISO, PORT, disk=disk,
            extra_args=["-serial", "file:" + SERIAL.replace("/", "\\")])

def do(line, wait=1.0):
    type_text(s, line, 0.01); sendkey(s, "ret", wait)

try:
    force_text(s)
    if not wait_for_prompt(s, timeout=120.0):
        print("FATAL no prompt"); sys.exit(3)
    do("run /bin/gfxdesk.ce", 1.5)
    time.sleep(1.0)                      # let it enter loop + first delay
    for k in ("1", "2"):                 # exercise pollkey / window focus
        sendkey(s, k, 0.4)
    sendkey(s, "q", 0.6)                 # quit -> kernel asks for TEXT consent
    sendkey(s, "y", 1.2)                 # grant consent, land in TEXT buffer
    time.sleep(0.5)
    t = screen_text(s)
    print("SCREEN has exit marker:", "exited to text shell" in t)
finally:
    kill_qemu(p)
    time.sleep(0.3)

print("==== SERIAL LOG ====")
if os.path.isfile(SERIAL):
    data = open(SERIAL, "r", errors="replace").read()
    lines = [ln for ln in data.splitlines() if ln.startswith("[")]
    print("---- first 40 ----")
    for ln in lines[:40]:
        print(ln)
    print("---- last 20 ----")
    for ln in lines[-20:]:
        print(ln)
    print("---- counts ----")
    for tag in ("[sleep", "[woke", "[wake", "[idle->", "[idle<-", "[tick3"):
        print("%-10s %d" % (tag, data.count(tag)))
else:
    print("NO SERIAL FILE")
