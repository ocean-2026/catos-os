#!/usr/bin/env python3
"""Route B: boot ISO + /tmp raw disk, format volume, install builtins,
run blkgen.ce, capture console screenshot + serial verdict.

The /tmp image lives on overlay (not the hpvs_fs home), avoiding the flock
rejection that blocks -hda on /home/user.
"""
import os, socket, subprocess, time, sys, struct, zlib

QEMU = "/home/user/.local/bin/qemu-system-i386"
SHARE = "/home/user/qemu-local/root/usr/share/qemu"
ISO = "/home/user/work/gui-tools/kernel_v25/catos.iso"
RUN = "/home/user/work/gui-tools/artifacts/gui-tools/run"
DISK = "/tmp/blktest.img"
PORT = 4781

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
               "/": "slash", "\n": "ret", "_": "shift-minus"}
    for ch in text:
        k = special.get(ch)
        if k is None:
            k = "shift-%s" % ch.lower() if ch.isupper() else ch
        sendkey(k, wait)


def boot():
    global proc, sock
    serial = os.path.join(RUN, "serial-ce.log")
    if os.path.exists(serial):
        os.remove(serial)
    cmdline = [QEMU, "-L", SHARE, "-m", "256", "-no-reboot", "-net", "none",
               "-cdrom", ISO, "-boot", "d", "-display", "none", "-cpu", "qemu32",
               "-drive", "file=%s,format=raw,if=ide" % DISK,
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
    for i in range(30):
        time.sleep(2)
        if os.path.exists(serial) and b"shell prompt ready" in open(serial, "rb").read():
            print("prompt ready after", (i + 1) * 2, "s")
            return


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
    data = open(ppm, "rb").read()
    parts, pos = [], 2
    while len(parts) < 3:
        while pos < len(data) and data[pos:pos + 1].isspace():
            pos += 1
        start = pos
        while pos < len(data) and not data[pos:pos + 1].isspace():
            pos += 1
        parts.append(int(data[start:pos]))
    w, h, _ = parts
    raw = data[pos + 1:w * h * 3 + pos + 1]

    def chunk(t, p):
        return struct.pack(">I", len(p)) + t + p + \
            struct.pack(">I", zlib.crc32(t + p) & 0xffffffff)
    scan = b"".join(b"\x00" + raw[y * w * 3:(y + 1) * w * 3] for y in range(h))
    with open(png, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(scan, 6)))
        f.write(chunk(b"IEND", b""))
    os.remove(ppm)
    print("shot:", png)


def main():
    boot()
    # format the volume (y/n confirm), then builtins seed automatically.
    type_text("format")
    sendkey("ret", 1.0)
    sendkey("y", 1.0)
    sendkey("ret", 10.0)
    # run the CE probe app
    type_text("blkgen")
    sendkey("ret", 1.0)
    time.sleep(4)
    dump("qemu-ce")
    time.sleep(1)
    try:
        sock.sendall(b"quit\n")
    except Exception:
        pass
    try:
        proc.wait(timeout=5)
    except Exception:
        proc.kill()


if __name__ == "__main__":
    main()
