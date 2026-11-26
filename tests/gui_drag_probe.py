"""Deterministic GUI drag self-check (kernel-side injection, no screendump).

Runs `gui deny` and `gui drag` self-tests over the serial console:
  - gui deny: asserted DRAG_TITLE sample block MUST move on injected drag,
              while hard-denied blocks must NOT. Line: GUI:deny ... sample_moved=1 result=1
  - gui drag: drag damage/layout constraints.              Line: GUI:drag ... result=1
This bypasses QEMU screendump timing / mouse coordinate drift entirely, giving
a deterministic PASS/FAIL for the drag *function* (as opposed to mouse_smoke,
which samples PPM frames and can catch a transient 640x400 text-mode frame).
"""
import os
import sys
import time
import subprocess
import socket

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qemu_driver as q

ROOT = os.path.dirname(HERE)
ISO = os.path.join(ROOT, "kernel_v25", "catos.iso")
OUT = os.path.join(ROOT, "artifacts", "gui-drag-probe")
os.makedirs(OUT, exist_ok=True)
PORT = 4542


def boot_serial(serial):
    vga = os.environ.get("VGA", "std")
    cmd = [q.find_qemu(), "-m", "256", "-smp", "1", "-cpu", "qemu32",
           "-vga", vga, "-no-reboot", "-net", "none", "-display", "none",
           "-boot", "order=d", "-cdrom", ISO,
           "-serial", "file:" + serial,
           "-monitor", "tcp:127.0.0.1:%d,server,nowait" % PORT]
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    for _ in range(40):
        if proc.poll() is not None:
            raise RuntimeError("qemu exited rc=%r" % proc.returncode)
        try:
            return proc, socket.create_connection(("127.0.0.1", PORT), timeout=5)
        except OSError:
            time.sleep(2)
    q.kill_qemu(proc)
    raise RuntimeError("no monitor")


serial = os.path.join(OUT, "serial.log")
if os.path.exists(serial):
    os.remove(serial)
proc, sock = boot_serial(serial)
try:
    q.force_text(sock)
    if not q.wait_for_prompt(sock, timeout=120.0):
        raise RuntimeError("no shell prompt")
    for sub in ("deny", "drag"):
        q.type_text(sock, "gui " + sub)
        q.sendkey(sock, "ret", 1.0)
        time.sleep(3.0)
finally:
    q.kill_qemu(proc)

raw = open(serial, "r", encoding="utf-8", errors="replace").read()
open(os.path.join(OUT, "serial-copy.txt"), "w", encoding="utf-8").write(raw)

results = []
def check(name, needle, must_have):
    line = next((ln for ln in raw.splitlines() if needle in ln), None)
    ok = line is not None and all(m in line for m in must_have)
    print("=== %s === %s :: %s" % (name, "PASS" if ok else "FAIL", line))
    results.append((name, ok))

check("gui deny", "GUI:deny", ["sample_moved=1", "result=1"])
check("gui drag", "GUI:drag", ["result=1"])

n = sum(1 for _, ok in results if ok)
print("SUMMARY %d/%d PASS" % (n, len(results)))
for name, ok in results:
    print("  %-10s %s" % (name, "PASS" if ok else "FAIL"))
if n != len(results):
    raise SystemExit(1)
