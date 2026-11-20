"""btop in gfx-default (40x12) mode: capture frame, check no wrap artifacts."""
import os, sys, time, struct, zlib
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qemu_driver import find_qemu, boot, kill_qemu, cmd, sendkey, type_text

ISO = os.path.join(HERE, "..", "kernel_v25", "catos.iso")
SHOTS = os.path.join(HERE, "qemu_shots_btop")
PORT = 4493
os.makedirs(SHOTS, exist_ok=True)

def ppm_to_png(ppm, png):
    data = open(ppm, "rb").read()
    assert data[:2] == b"P6"
    parts, pos = [], 2
    while len(parts) < 3:
        while data[pos:pos+1].isspace(): pos += 1
        if data[pos:pos+1] == b"#":
            while data[pos] not in (10, 13): pos += 1
            continue
        st = pos
        while not data[pos:pos+1].isspace(): pos += 1
        parts.append(int(data[st:pos]))
    pos += 1
    w, h, _ = parts
    raw = data[pos:pos + w*h*3]
    def ck(t, p):
        return struct.pack(">I", len(p)) + t + p + struct.pack(">I", zlib.crc32(t+p) & 0xffffffff)
    scan = b"".join(b"\x00" + raw[y*w*3:(y+1)*w*3] for y in range(h))
    open(png, "wb").write(b"\x89PNG\r\n\x1a\n"
        + ck(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
        + ck(b"IDAT", zlib.compress(scan, 6)) + ck(b"IEND", b""))
    os.remove(ppm)

disk = os.path.join(SHOTS, "gfx_disk.img")
if not os.path.exists(disk): open(disk, "wb").truncate(64*1024*1024)
p, s = boot(find_qemu(), ISO, PORT, disk=disk, log=os.path.join(SHOTS, "gfx_qemu.log"))
try:
    time.sleep(9)
    type_text(s, "format"); sendkey(s, "ret"); time.sleep(0.4)
    sendkey(s, "y"); sendkey(s, "ret"); time.sleep(4)
    type_text(s, "btop"); sendkey(s, "ret"); time.sleep(1.2)
    ppm = os.path.join(SHOTS, "btop_gfx.ppm")
    png = os.path.join(SHOTS, "btop_gfx.png")
    cmd(s, "screendump %s" % ppm.replace("\\", "/"), 1.0)
    for _ in range(30):
        if os.path.exists(ppm) and os.path.getsize(ppm) > 0: break
        time.sleep(0.5)
    ppm_to_png(ppm, png)
    sendkey(s, "ret"); time.sleep(0.5)
    print("GFX btop frame:", png)
finally:
    kill_qemu(p)
