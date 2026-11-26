"""Live QEMU probe: FAT32 write survives power cycle (disk persistence).

Boots a fixed raw disk image (NOT via qemu_driver staging, so both boots
share the same backing file): first boot formats and writes a marker with
`echo ... > /PERSIST.TXT`, powers off cleanly; second boots and cats it back.
PASS requires the marker text to survive the power cycle.
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
OUT = os.path.join(ROOT, "artifacts", "disk-persist-probe")
os.makedirs(OUT, exist_ok=True)
DISK = os.path.join(OUT, "persist.img")
if not os.path.exists(DISK):
    open(DISK, "wb").truncate(64 * 1024 * 1024)
MARK = "CATOS_PERSIST_V281_7F3A"
PORT = 4540


def boot_fixed(port):
    """Start QEMU against the FIXED disk path (no staging copy)."""
    cmdline = [q.find_qemu(), "-m", "256", "-no-reboot", "-net", "none",
               "-cdrom", ISO, "-boot", "order=d", "-display", "none",
               "-monitor", "tcp:127.0.0.1:%d,server,nowait" % port,
               "-drive", "file=%s,format=raw,if=ide,index=0,media=disk" % DISK,
               "-D", os.path.join(OUT, "qemu%d.log" % port)]
    proc = subprocess.Popen(cmdline, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    for _ in range(40):
        if proc.poll() is not None:
            raise RuntimeError("qemu exited rc=%r" % proc.returncode)
        try:
            return proc, socket.create_connection(("127.0.0.1", port), timeout=5)
        except OSError:
            time.sleep(2)
    q.kill_qemu(proc)
    raise RuntimeError("no monitor on port %d" % port)


def enter_text(s):
    time.sleep(9)
    q.type_text(s, "text"); q.sendkey(s, "ret"); time.sleep(0.6)
    q.sendkey(s, "y"); q.sendkey(s, "ret"); time.sleep(1.5)
    if not q.wait_for_prompt(s, 40):
        raise RuntimeError("no shell prompt after text switch")


results = []
# ---- boot 1: format, write marker, clean poweroff ----
p, s = boot_fixed(PORT)
try:
    enter_text(s)
    q.type_text(s, "format"); q.sendkey(s, "ret"); time.sleep(0.4)
    q.sendkey(s, "y"); q.sendkey(s, "ret"); time.sleep(6)
    # write marker via builtin echo redirection
    q.type_text(s, "echo %s > /PERSIST.TXT" % MARK)
    q.sendkey(s, "ret"); time.sleep(1.0)
    # clear first so the marker can only come from the file readback,
    # not from the echo command echo on the screen.
    q.type_text(s, "clear"); q.sendkey(s, "ret"); time.sleep(0.3)
    q.type_text(s, "edit -c /PERSIST.TXT"); q.sendkey(s, "ret"); time.sleep(1.0)
    out1 = q.screen_text(s)
    open(os.path.join(OUT, "boot1.txt"), "w").write(out1)
    w1 = MARK in out1
    print("=== boot1 write === %s" % ("PASS" if w1 else "FAIL"))
    results.append(("boot1_write", w1))
    # clean shutdown so FAT buffers flush. The monitor socket drops as soon
    # as QEMU powers off -> ConnectionResetError is the expected signal.
    try:
        q.type_text(s, "poweroff"); q.sendkey(s, "ret"); time.sleep(2)
    except (ConnectionResetError, OSError):
        print("=== boot1 poweroff: monitor dropped (expected) ===")
finally:
    q.kill_qemu(p)
time.sleep(2)

# ---- boot 2: read marker back ----
p, s = boot_fixed(PORT + 1)
try:
    enter_text(s)
    # NO format this time: read whatever survived on the disk
    q.type_text(s, "clear"); q.sendkey(s, "ret"); time.sleep(0.3)
    q.type_text(s, "edit -c /PERSIST.TXT"); q.sendkey(s, "ret"); time.sleep(1.5)
    out2 = q.screen_text(s)
    open(os.path.join(OUT, "boot2.txt"), "w").write(out2)
    r2 = MARK in out2
    print("=== boot2 readback === %s" % ("PASS" if r2 else "FAIL"))
    results.append(("boot2_readback", r2))
    # dump tail for diagnosis
    print(out2[-1200:])
finally:
    q.kill_qemu(p)

n = sum(1 for _, ok in results if ok)
print("SUMMARY %d/%d PASS" % (n, len(results)))
for name, ok in results:
    print("  %-14s %s" % (name, "PASS" if ok else "FAIL"))
if n != len(results):
    raise SystemExit(1)
