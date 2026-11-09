#!/usr/bin/env python3
"""qa_probe_final.py -- QA final-verification probe driver (QA-2 / Yan).

Drives a CatOS guest through the QEMU HMP monitor and captures the
CONSOLE TEXT of three real-machine probes:

    run /bin/allocprobe.ce   -> the printed heap addresses
    run /bin/heaptest.ce     -> "HEAPTEST PASS" / "HEAPTEST FAIL"
    run /bin/go.ce           -> "fib(10)= 55" / "demo done" / "go: exit ok"

Two capture channels per probe:

  1. TEXT  -- the guest is in VGA text mode (80x25), so the screen
              characters live in the text buffer at physical 0xB8000.
              HMP `pmemsave 0xb8000 4000 <file>` dumps it verbatim and
              we decode char/attribute cells.  This yields the EXACT
              bytes the guest printed -- no OCR, no guessing.
  2. IMAGE -- HMP `screendump` writes a PPM; a dependency-free
              PPM->PNG converter (copied from tests/qemu_regress.py)
              turns it into a viewable PNG under tests/qemu_shots/.

Isolation notes (this driver must not disturb a parallel QA run):
  * the QEMU executable is passed in with --qemu and is expected to be a
    RENAMED COPY, so a `taskkill /IM qemu-system-i386.exe` by another
    driver cannot hit us;
  * the HMP monitor port is 4455, not the repo default 4444;
  * the guest is terminated by PID (`taskkill /F /PID`), never by image
    name.

usage:
  python tests/qa_probe_final.py <catos.iso> [--qemu PATH] [--disk PATH]
"""
import os
import socket
import struct
import subprocess
import sys
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
SHOTS = os.path.join(HERE, "qemu_shots")
MONITOR = ("127.0.0.1", 4455)
TEXT_BASE = 0xB8000
TEXT_CELLS = 80 * 25


# ---------------------------------------------------------------- monitor --

def connect_monitor(timeout=60):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            return socket.create_connection(MONITOR, timeout=5)
        except OSError:
            time.sleep(1)
    raise RuntimeError("could not reach the QEMU monitor on %s:%d"
                       % MONITOR)


def hmp(sock, command, wait=0.5):
    """Send one HMP command and drain the reply."""
    sock.sendall((command + "\n").encode())
    time.sleep(wait)
    sock.setblocking(False)
    chunks = []
    try:
        while True:
            d = sock.recv(65536)
            if not d:
                break
            chunks.append(d)
    except (BlockingIOError, socket.timeout):
        pass
    finally:
        sock.setblocking(True)
    return b"".join(chunks).decode("latin-1")


# ------------------------------------------------------------------- keys --

SPECIAL = {" ": "spc", "-": "minus", "=": "equal", ".": "dot",
            "/": "slash", "\n": "ret", ":": "shift-semicolon"}


def sendkey(sock, key, wait=0.12):
    hmp(sock, "sendkey " + key, wait)


def type_text(sock, text, wait=0.12):
    for ch in text:
        key = SPECIAL.get(ch)
        if key is None:
            key = ("shift-" + ch.lower()) if ch.isupper() else ch
        sendkey(sock, key, wait)


def run_cmd(sock, cmdline, settle=2.0):
    type_text(sock, cmdline)
    sendkey(sock, "ret", settle)


# -------------------------------------------------------------- capturing --

def ppm_to_png(ppm, png):
    """Zero-dependency P6 PPM -> PNG (same converter as qemu_regress.py)."""
    with open(ppm, "rb") as f:
        data = f.read()
    if data[:2] != b"P6":
        raise ValueError("screendump did not produce a PPM (got %r)" % data[:2])
    parts, pos = [], 2
    while len(parts) < 3:
        while pos < len(data) and data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            while data[pos] not in (10, 13):
                pos += 1
            continue
        start = pos
        while pos < len(data) and not data[pos:pos + 1].isspace():
            pos += 1
        parts.append(int(data[start:pos]))
    pos += 1
    w, h, _ = parts
    raw = data[pos:pos + w * h * 3]
    if len(raw) < w * h * 3:
        raise ValueError("PPM pixel data truncated")

    def chunk(typ, payload):
        return (struct.pack(">I", len(payload)) + typ + payload +
                struct.pack(">I", zlib.crc32(typ + payload) & 0xffffffff))

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    stride = w * 3
    scan = b"".join(b"\x00" + raw[y * stride:(y + 1) * stride]
                    for y in range(h))
    with open(png, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", ihdr))
        f.write(chunk(b"IDAT", zlib.compress(scan, 6)))
        f.write(chunk(b"IEND", b""))
    os.remove(ppm)


def screendump(sock, name):
    ppm = os.path.join(SHOTS, name + ".ppm")
    png = os.path.join(SHOTS, name + ".png")
    if os.path.exists(ppm):
        os.remove(ppm)
    hmp(sock, "screendump %s" % ppm.replace("\\", "/"), 1.5)
    ppm_to_png(ppm, png)
    return png


def read_text(sock):
    """Dump the VGA text buffer at 0xB8000 and decode it to 25 lines."""
    raw = os.path.join(SHOTS, "_textbuf.bin")
    if os.path.exists(raw):
        os.remove(raw)
    hmp(sock, "pmemsave 0x%x %d %s" % (TEXT_BASE, TEXT_CELLS * 2,
                                        raw.replace("\\", "/")), 1.2)
    if not os.path.exists(raw):
        return ["<no text buffer dump>"]
    with open(raw, "rb") as f:
        data = f.read()
    os.remove(raw)
    if len(data) < TEXT_CELLS * 2:
        return ["<text buffer truncated: %d bytes>" % len(data)]
    lines = []
    for row in range(25):
        chars = []
        for col in range(80):
            cell = data[(row * 80 + col) * 2:(row * 80 + col) * 2 + 2]
            ch = cell[0]
            if ch == 0:
                ch = 0x20
            if 0x80 <= ch <= 0xFF:
                ch = 0x3F              # '?' for CJK / wide-cell markers
            chars.append(chr(ch))
        lines.append("".join(chars).rstrip())
    return lines


def show(label, lines, keep=()):
    print("\n===== %s : VGA text buffer (0xB8000) =====" % label)
    for i, ln in enumerate(lines):
        if ln.strip():
            print("  %2d| %s" % (i, ln))
    print("===== %s : end =====" % label)
    for pat in keep:
        hit = [i for i, ln in enumerate(lines) if pat in ln]
        print("  PATTERN %-28r -> %s" % (pat, ("rows " + str(hit)) if hit
                                            else "NOT FOUND"))


# ------------------------------------------------------------------- boot --

def boot(iso, qemu, disk):
    cmd = [qemu, "-m", "256", "-no-reboot", "-net", "none",
           "-cdrom", iso, "-boot", "order=d",
           "-display", "none",
           "-monitor", "tcp:%s:%d,server,nowait" % MONITOR,
           "-drive", "file=%s,format=raw,if=ide" % disk]
    print("boot:", " ".join(cmd))
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    return proc


def kill_by_pid(proc):
    if proc.poll() is None:
        subprocess.run(["taskkill", "/F", "/PID", str(proc.pid)],
                       capture_output=True)
        try:
            proc.wait(timeout=10)
        except Exception:
            pass


# ------------------------------------------------------------------- main --

def main():
    iso = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(HERE), "kernel_v25", "catos.iso")
    qemu = r"C:\qemu-portable\qemu-system-i386.exe"
    disk = None
    for i, a in enumerate(sys.argv):
        if a == "--qemu" and i + 1 < len(sys.argv):
            qemu = sys.argv[i + 1]
        if a == "--disk" and i + 1 < len(sys.argv):
            disk = sys.argv[i + 1]
    if disk is None:
        disk = os.path.join(HERE, "qa_probe_disk.img")
    if not os.path.exists(disk):
        with open(disk, "wb") as f:
            f.truncate(64 * 1024 * 1024)
    os.makedirs(SHOTS, exist_ok=True)

    proc = boot(iso, qemu, disk)
    rc = 1
    try:
        sock = connect_monitor()
        print("monitor connected on %s:%d" % MONITOR)
        time.sleep(30)          # TCG boot: GRUB alone takes ~10 s

        # --- format the FAT32 volume so /bin exists and the kernel
        #     installs the ISO's built-in apps onto it ---
        run_cmd(sock, "format", 1.5)
        sendkey(sock, "y", 0.5)
        sendkey(sock, "ret", 12.0)     # fs_format(64 MiB) + builtin install
        show("after format", read_text(sock))

        probes = [
            ("allocprobe", "run /bin/allocprobe.ce",
             ("ALLOCPROBE PASS", "ALLOCPROBE FAIL", "alloc(64) = ",
              "alloc(8k) = ")),
            ("heaptest", "run /bin/heaptest.ce",
             ("HEAPTEST PASS", "HEAPTEST FAIL")),
            ("go", "run /bin/go.ce",
             ("fib(10)= 55", "demo done", "go: exit ok", "go: ")),
        ]
        for name, cmdline, pats in probes:
            run_cmd(sock, cmdline, 1.0)
            time.sleep(6.0)       # let the guest run the CE program
            screendump(sock, "qa2_%s" % name)
            show(name, read_text(sock), keep=pats)

        rc = 0
    except Exception as exc:                       # noqa: BLE001
        print("PROBE ERROR: %s" % exc)
        rc = 3
    finally:
        kill_by_pid(proc)
    print("\nqa_probe_final exit=%d" % rc)
    return rc


if __name__ == "__main__":
    sys.exit(main())
