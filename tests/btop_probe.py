"""Live probe: btop dashboard renders and exits cleanly."""
import os, sys, time, struct, zlib
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qemu_driver import (find_qemu, boot, kill_qemu, cmd, sendkey,
                         type_text, wait_for_prompt, screen_text)

ISO = os.path.join(HERE, "..", "kernel_v25", "catos.iso")
SHOTS = os.path.join(HERE, "qemu_shots_btop")
PORT = 4492
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

def snap(s, name):
    ppm = os.path.join(SHOTS, name + ".ppm")
    png = os.path.join(SHOTS, name + ".png")
    if os.path.exists(ppm): os.remove(ppm)
    cmd(s, "screendump %s" % ppm.replace("\\", "/"), 1.0)
    for _ in range(30):
        if os.path.exists(ppm) and os.path.getsize(ppm) > 0: break
        time.sleep(0.5)
    ppm_to_png(ppm, png)
    return png

disk = os.path.join(SHOTS, "disk.img")
if not os.path.exists(disk):
    open(disk, "wb").truncate(64*1024*1024)
p, s = boot(find_qemu(), ISO, PORT, disk=disk, log=os.path.join(SHOTS, "qemu.log"))
try:
    # gfx is the default -> the VGA text buffer is blank, so wait_for_prompt
    # cannot see the prompt.  Boot takes ~4s (GRUB) + kernel init; sleep,
    # then switch to text mode BLIND (keystrokes work in any video mode).
    time.sleep(9)
    type_text(s, "text"); sendkey(s, "ret"); time.sleep(0.6)
    sendkey(s, "y"); sendkey(s, "ret"); time.sleep(1.5)
    if not wait_for_prompt(s, 30):
        raise RuntimeError("no shell prompt after text switch")
    type_text(s, "format"); sendkey(s, "ret"); time.sleep(0.4)
    sendkey(s, "y"); sendkey(s, "ret"); time.sleep(4)
    before = screen_text(s)
    type_text(s, "btop"); sendkey(s, "ret"); time.sleep(0.8)
    during = screen_text(s)
    snap(s, "btop_frame")
    sendkey(s, "ret"); time.sleep(1.0)   # any key quits
    after = screen_text(s)
    print("BTOP FRAME:\n", during[:1400])
    assert "btop" in during, "btop header missing"
    assert "cpu0" in during, "per-core row missing"
    assert "MEM" in during, "memory panel missing"
    assert "DSK" in during and "NET" in during, "disk/net panel missing"
    assert "TID" in during, "task table missing"
    assert "btop" not in after.splitlines()[-1], "btop did not exit"
    print("BTOP LIVE PROBE PASS")
finally:
    kill_qemu(p)
