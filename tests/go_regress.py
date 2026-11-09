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
        return explicit if os.path.isfile(explicit) else explicit
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

    The kernel's text back-end writes 0xB8000 in 80x25 cells of
    (char, attribute).  xp prints the bytes as hex; we keep every second
    byte of each cell, i.e. the character.

    Parsing notes (learned the hard way):
      * `xp /N 0xADDR` prints one line per 16 bytes, each line shaped
            000b8000: 0x0b200b20 0x0b5f0b20 0x0b5f0b5f 0x0b5f0b5f
        i.e. FOUR 32-bit words per line, each word holding FOUR bytes
        in LITTLE-ENDIAN order (low address first).
      * The monitor also echoes the command line back into the socket,
        mangled ("xxpxp xp /4000 0xb8000"), so only lines that start
        with an 8-hex-digit address are memory dumps.

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
            continue                      # monitor echo / prompt noise
        for word in re.findall(r"0x([0-9a-fA-F]{8})", line):
            v = int(word, 16)
            # little-endian: the lowest byte is the lowest address
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
        """Blank the screen so the next command's output is unambiguous."""
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

    step("format", step_format)
    step("ls-bin", step_ls_bin)
    step("go-demo", step_go_demo)
    step("go-test", step_go_test)
    step("go-syntax-err", step_go_syn)
    step("node-test", step_node_test)

    if "--keep" not in args:
        kill_qemu()
    fails = [r for r in results if not r[1]]
    print("\nsteps=%d ok=%d fail=%d" % (len(results),
                                        len(results) - len(fails), len(fails)))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
