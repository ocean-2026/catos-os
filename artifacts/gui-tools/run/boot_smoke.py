#!/usr/bin/env python3
"""Smoke: boot the freshly built ISO-only, wait for shell prompt, dump screen."""
import os, socket, subprocess, time, sys, re

QEMU = "/home/user/.local/bin/qemu-system-i386"
SHARE = "/home/user/qemu-local/root/usr/share/qemu"
ISO = "/home/user/work/gui-tools/kernel_v25/catos.iso"
RUN = "/home/user/work/gui-tools/artifacts/gui-tools/run"
PORT = 4780

VGA_TEXT = 0xB8000

def read_screen(s):
    nbytes = 80 * 25 * 2
    s.sendall(("xp /%d 0x%x\n" % (nbytes, VGA_TEXT)).encode())
    time.sleep(2.0)
    s.setblocking(False)
    chunks = []
    while True:
        try:
            b = s.recv(65536)
        except BlockingIOError:
            break
        if not b: break
        chunks.append(b)
    s.setblocking(True)
    data = bytearray()
    for line in b"".join(chunks).splitlines():
        line = line.strip()
        if not re.match(r"^[0-9a-fA-F]{8,16}:", line.decode("latin-1")):
            continue
        for word in re.findall(r"0x([0-9a-fA-F]{8})", line.decode("latin-1")):
            v = int(word, 16)
            data += bytes((v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF,
                           (v >> 24) & 0xFF))
    rows = []
    for r in range(25):
        chars = []
        base = r * 80 * 2
        for c in range(80):
            off = base + c * 2
            if off < len(data):
                chars.append(chr(data[off]) if 32 <= data[off] < 127 else " ")
        rows.append("".join(chars).rstrip())
    return "\n".join(rows)

def main():
    os.makedirs(RUN, exist_ok=True)
    serial = os.path.join(RUN, "serial-boot.log")
    cmdline = [QEMU, "-L", SHARE, "-m", "256", "-no-reboot", "-net", "none",
               "-cdrom", ISO, "-boot", "d", "-display", "none", "-cpu", "qemu32",
               "-monitor", "tcp:127.0.0.1:%d,server,nowait" % PORT,
               "-serial", "file:" + serial]
    print("boot:", " ".join(cmdline))
    proc = subprocess.Popen(cmdline, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    sock = None
    for _ in range(20):
        if proc.poll() is not None:
            print("qemu exited rc=%r" % proc.returncode); sys.exit(1)
        try:
            sock = socket.create_connection(("127.0.0.1", PORT), timeout=5)
            break
        except OSError:
            time.sleep(2)
    if sock is None:
        print("no monitor"); sys.exit(1)

    pat = re.compile(r"catos:[^\n]*[\$#]")
    for i in range(30):
        time.sleep(2)
        txt = read_screen(sock)
        if pat.search(txt):
            print("PROMPT FOUND after", (i+1)*2, "s")
            print(txt)
            break
        print("...waiting", (i+1)*2, "s")
    else:
        print("NO PROMPT; screen:")
        print(read_screen(sock))

    sock.sendall(b"quit\n")
    try: proc.wait(timeout=5)
    except Exception: proc.kill()

if __name__ == "__main__":
    main()
