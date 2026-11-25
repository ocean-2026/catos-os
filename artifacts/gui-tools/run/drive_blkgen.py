#!/usr/bin/env python3
"""Drives the built ISO through the three blkgen routes under QEMU (ISO-only).

Usage:
  python3 drive_blkgen.py boot            # boot and leave running (debug)
  python3 drive_blkgen.py shots           # app + calm built-in page shots
  python3 drive_blkgen.py ce               # run blkgen.ce, capture verdict
  python3 drive_blkgen.py all             # shots then ce in one guest

Screendumps are PPM P6 800x600; converted to PNG with hand-built chunks.
Serial BLKGEN geometry lines are harvested from the -serial file.
"""
import os, socket, subprocess, time, sys, re, struct, zlib

QEMU = "/home/user/.local/bin/qemu-system-i386"
SHARE = "/home/user/qemu-local/root/usr/share/qemu"
ISO = "/home/user/work/gui-tools/kernel_v25/catos.iso"
RUN = "/home/user/work/gui-tools/artifacts/gui-tools/run"
PORT = 4780

proc = None
sock = None


def cmd(c, wait=0.5):
    sock.sendall((c + "\n").encode())
    time.sleep(wait)
    sock.setblocking(False)
    while True:
        try:
            if not sock.recv(65536):
                break
        except BlockingIOError:
            break
    sock.setblocking(True)


def sendkey(k, wait=0.12):
    cmd("sendkey " + k, wait)


def type_text(text, wait=0.1):
    special = {" ": "spc", "-": "minus", "=": "equal", ".": "dot",
               "/": "slash", "\n": "ret", "_": "shift-minus",
               ":": "shift-semicolon"}
    for ch in text:
        k = special.get(ch)
        if k is None:
            k = "shift-%s" % ch.lower() if ch.isupper() else ch
        sendkey(k, wait)


def boot():
    global proc, sock
    os.makedirs(RUN, exist_ok=True)
    serial = os.path.join(RUN, "serial-run.log")
    if os.path.exists(serial):
        os.remove(serial)
    cmdline = [QEMU, "-L", SHARE, "-m", "256", "-no-reboot", "-net", "none",
               "-cdrom", ISO, "-boot", "d", "-display", "none", "-cpu", "qemu32",
               "-monitor", "tcp:127.0.0.1:%d,server,nowait" % PORT,
               "-serial", "file:" + serial]
    print("boot:", " ".join(cmdline))
    proc = subprocess.Popen(cmdline, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    for _ in range(20):
        if proc.poll() is not None:
            raise RuntimeError("qemu exited rc=%r" % proc.returncode)
        try:
            sock = socket.create_connection(("127.0.0.1", PORT), timeout=5)
            break
        except OSError:
            time.sleep(2)
    if sock is None:
        raise RuntimeError("no monitor")
    # wait for shell prompt ready on serial
    for i in range(30):
        time.sleep(2)
        if os.path.exists(serial):
            if b"shell prompt ready" in open(serial, "rb").read():
                print("prompt ready after", (i + 1) * 2, "s")
                return
    print("WARN: prompt ready not seen on serial")


def wait_prompt_serial(marker, timeout=30):
    serial = os.path.join(RUN, "serial-run.log")
    for _ in range(timeout):
        time.sleep(1)
        if os.path.exists(serial) and marker in open(serial, "rb").read().decode("latin-1", "replace"):
            return True
    return False


def dump(name):
    ppm = os.path.join(RUN, name + ".ppm")
    png = os.path.join(RUN, name + ".png")
    if os.path.exists(ppm):
        os.remove(ppm)
    cmd("screendump " + ppm, 0.5)
    for _ in range(40):
        if os.path.exists(ppm) and os.path.getsize(ppm) > 0:
            break
        time.sleep(0.5)
    else:
        raise RuntimeError("no screendump for " + name)
    ppm_to_png(ppm, png)
    print("shot:", png)
    return png


def ppm_to_png(ppm, png):
    data = open(ppm, "rb").read()
    if data[:2] != b"P6":
        raise ValueError("not PPM P6: %r" % data[:2])
    parts, pos = [], 2
    while len(parts) < 3:
        while pos < len(data) and data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            while pos < len(data) and data[pos] not in (10, 13):
                pos += 1
            continue
        start = pos
        while pos < len(data) and not data[pos:pos + 1].isspace():
            pos += 1
        parts.append(int(data[start:pos]))
    pos += 1
    w, h, _ = parts
    raw = data[pos:pos + w * h * 3]

    def chunk(typ, payload):
        return struct.pack(">I", len(payload)) + typ + payload + \
            struct.pack(">I", zlib.crc32(typ + payload) & 0xffffffff)

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


def stage_shots():
    # app style
    type_text("gui shot-style-app blkgen")
    sendkey("ret", 1.0)
    time.sleep(3)
    dump("qemu-app")
    sendkey("esc", 1.0)
    time.sleep(1.5)
    # calm style
    type_text("gui shot-style-calm blkgen")
    sendkey("ret", 1.0)
    time.sleep(3)
    dump("qemu-calm")
    sendkey("esc", 1.0)
    time.sleep(1.5)


def stage_ce():
    type_text("blkgen")
    sendkey("ret", 1.0)
    time.sleep(3)
    dump("qemu-ce")


def finish():
    try:
        sock.sendall(b"quit\n")
    except Exception:
        pass
    try:
        proc.wait(timeout=5)
    except Exception:
        proc.kill()


if __name__ == "__main__":
    what = sys.argv[1] if len(sys.argv) > 1 else "all"
    try:
        boot()
        if what in ("shots", "all"):
            stage_shots()
        if what in ("ce", "all"):
            stage_ce()
    finally:
        finish()
