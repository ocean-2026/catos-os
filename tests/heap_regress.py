#!/usr/bin/env python3
"""heap_regress.py -- on-metal acceptance for the P0-1 / P0-2 heap fixes.

Two CE probes are installed in the kernel image as built-ins:

  /bin/allocprobe.ce  (V-5)  proves api->alloc() returns a usable,
                               non-NULL address inside the heap window
  /bin/heaptest.ce    (V-6)  proves an allocation may cross page
                               boundaries without a #PF

Both print their verdict as the LAST line of their output, so the check
is a plain substring search on the text screen.

The screen is read through the QEMU monitor's physical-memory dump of
the VGA text buffer ("xp /4000 0xb8000") instead of being OCR-ed from a
screenshot -- the frame buffer holds (char, attribute) pairs, so the
character is every second byte.

usage:
  python tests/heap_regress.py <catos.iso> [--qemu PATH] [--keep]
"""
import os
import re
import socket
import subprocess
import sys
import time

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


def find_qemu(explicit=None):
    if explicit:
        return explicit
    for c in DEFAULT_QEMU_CANDIDATES:
        if os.path.isfile(c):
            return c
    for d in (os.environ.get("PATH") or "").split(os.pathsep):
        p = os.path.join(d, "qemu-system-i386.exe")
        if os.path.isfile(p):
            return p
    return None


def kill_qemu():
    subprocess.run(["taskkill", "/F", "/IM", "qemu-system-i386.exe"],
                   capture_output=True)


def boot(iso, qemu, disk):
    kill_qemu()
    time.sleep(1)
    cmd = [qemu, "-m", "256", "-no-reboot", "-net", "none",
           "-cdrom", iso, "-boot", "order=d",
           "-display", "none",
           "-monitor", "tcp:%s:%d,server,nowait" % MONITOR]
    if disk:
        cmd += ["-drive", "file=%s,format=raw,if=ide" % disk]
    print("boot:", " ".join(cmd))
    subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(20):
        try:
            return socket.create_connection(MONITOR, timeout=5)
        except OSError:
            time.sleep(2)
    raise RuntimeError("could not reach the QEMU monitor")


def cmd(s, c, wait=0.5):
    """Send one HMP command and return everything it printed."""
    s.sendall((c + "\n").encode())
    time.sleep(wait)
    chunks = []
    try:
        s.setblocking(False)
        time.sleep(0.15)
        while True:
            try:
                b = s.recv(65536)
            except BlockingIOError:
                break
            if not b:
                break
            chunks.append(b)
    finally:
        s.setblocking(True)
    return b"".join(chunks).decode("latin-1", "replace")


def sendkey(s, key, wait=0.12):
    cmd(s, "sendkey " + key, wait)


def type_text(s, text, wait=0.12):
    special = {" ": "spc", "-": "minus", "=": "equal", ".": "dot",
               "/": "slash", "\n": "ret"}
    for ch in text:
        k = special.get(ch)
        if k is None:
            k = "shift-%s" % ch.lower() if ch.isupper() else ch
        sendkey(s, k, wait)


def read_screen(s):
    """Read the VGA text frame buffer through the monitor's xp dump.

    `xp /N 0xADDR` prints one line per 16 bytes:

        000b8000: 0x0b200b20 0x0b5f0b20 0x0b5f0b5f 0x0b5f0b5f

    i.e. FOUR 32-bit words per line, each holding FOUR bytes in
    LITTLE-ENDIAN order (lowest address first).  The monitor also echoes
    the command back into the socket, mangled ("xxpxp xp /4000 0xb8000"),
    so only lines starting with an 8-hex-digit address are memory dumps.

    A 2-hex-digit regex would capture fragments such as "0x0b" out of
    "0x0b200b20" and scramble every character -- hence the explicit
    8-digit match plus little-endian unpacking.
    """
    nbytes = VGA_COLS * VGA_ROWS * 2
    out = cmd(s, "xp /%d 0x%x" % (nbytes, VGA_TEXT), wait=2.0)
    data = bytearray()
    for line in out.splitlines():
        line = line.strip()
        if not re.match(r"^[0-9a-fA-F]{8}:", line):
            continue
        for word in re.findall(r"0x([0-9a-fA-F]{8})", line):
            v = int(word, 16)
            data += bytes((v & 0xFF, (v >> 8) & 0xFF,
                           (v >> 16) & 0xFF, (v >> 24) & 0xFF))
    if not data:
        return ""
    rows = []
    for r in range(VGA_ROWS):
        chars = []
        base = r * VGA_COLS * 2
        for c in range(VGA_COLS):
            off = base + c * 2
            if off < len(data):
                chars.append(chr(data[off]) if 32 <= data[off] < 127 else " ")
        rows.append("".join(chars).rstrip())
    return "\n".join(rows)


def screen_text(s):
    """The screen as one blob with whitespace collapsed, for substring
    assertions that must survive line wrapping."""
    return re.sub(r"\s+", " ", read_screen(s))


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

    disk = os.path.join(HERE, "qemu_disk.img")
    if not os.path.exists(disk):
        with open(disk, "wb") as fh:
            fh.truncate(64 * 1024 * 1024)

    s = boot(iso, qemu, disk)
    print("monitor connected")
    time.sleep(25)                      # boot to shell (TCG is slow)

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
        do("cls", 1.0)

    def step_format():
        clear()
        do("format", 1.0)
        sendkey(s, "y", 0.4)
        sendkey(s, "ret", 10.0)          # fs_format(64 MiB) + install
        return True, "formatted"

    def step_ls_bin():
        clear()
        do("ls /bin", 2.0)
        txt = screen_text(s)
        # The FAT volume stores 8.3 names and the shell prints them back
        # through fat_name_show(): the base is truncated to 8 characters
        # and lower-cased.  "allocprobe.ce" therefore shows up as
        # "allocpro.ce" -- matching the long spelling would be wrong.
        ok = "allocpro.ce" in txt and "heaptest.ce" in txt
        return ok, "allocpro.ce=%s heaptest.ce=%s" % (
            "allocpro.ce" in txt, "heaptest.ce" in txt)

    def run_app(argv_line, expect, wait=6.0):
        clear()
        do(argv_line, wait)
        txt = screen_text(s)
        missing = [e for e in expect if e not in txt]
        return (not missing), ("missing=%r" % missing if missing else "all found")

    def step_allocprobe():
        return run_app("run /bin/allocprobe.ce",
                       ["ALLOCPROBE: alloc works"], wait=6.0)

    def step_heaptest():
        return run_app("run /bin/heaptest.ce",
                       ["HEAPTEST PASS"], wait=10.0)

    step("format", step_format)
    step("ls-bin", step_ls_bin)
    step("allocprobe", step_allocprobe)
    step("heaptest", step_heaptest)

    if "--keep" not in args:
        kill_qemu()
    fails = [r for r in results if not r[1]]
    print("\nsteps=%d ok=%d fail=%d" % (len(results),
                                        len(results) - len(fails), len(fails)))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
