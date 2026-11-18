#!/usr/bin/env python3
"""videomode_regress.py -- v2.56 boot display-mode regression.

Covers the /etc/videomode feature:

  a) no config file present -> boot straight into gfx24 (VBE24, 800x600x24)
  b) /etc/videomode = text  -> boot in text mode, prompt readable, no warning
  c) /etc/videomode = gfx | vbe -> that mode, or a safe text fallback if the
     back-end cannot probe on this VGA
  d) malformed content (over-long line, binary bytes, pure whitespace, unknown
     value) -> default gfx24, no crash
  e) no VBE/SVGA environment (-vga cirrus) -> fall back to text with a reason
     on screen; never a black silent boot or a panic.

The active back-end identity / geometry is read straight out of the guest's
`be` front-end pointer through the HMP monitor (`xp`), so the check works even
in a framebuffer mode where the legacy 0xB8000 text plane is blank.

usage:
  python tests/videomode_regress.py [catos.iso] --qemu /home/user/bin/qemu-system-i386
"""
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qemu_driver import (boot, cmd, find_qemu, kill_qemu,  # noqa: E402
                         screen_text, sendkey, type_text)

REPO = os.path.dirname(HERE)
ISO = os.path.join(REPO, "kernel_v25", "catos.iso")
PE = os.path.join(REPO, "kernel_v25", "build", "kernel.pe")
SHARED_DISK = os.path.join(REPO, "kernel_v25", "catos_disk.img")
PORT = 4510

MODE_NAME = {0: "text", 1: "gfx", 2: "vbe", 3: "vbe24"}


# --------------------------------------------------------------------------
# host-side FAT32 injection of /etc/videomode (geometry read from MBR/DBR)
# --------------------------------------------------------------------------
def write_videomode(img, content):
    """Write a small file at /etc/videomode onto a CatOS-formatted FAT32 img.

    Mirrors tests/inject_any.py: partition start from MBR LBA0, BPB from the
    DBR, both FAT copies updated, directory entry space-padded 8.3 short name.
    """
    with open(img, "r+b") as f:
        mbr = f.read(512)
        if mbr[510] != 0x55 or mbr[511] != 0xAA:
            raise RuntimeError("no MBR signature")
        part = int.from_bytes(mbr[454:458], "little")
        f.seek(part * 512)
        dbr = f.read(512)
        bps = int.from_bytes(dbr[0x0B:0x0D], "little")
        spc = dbr[0x0D]
        rsvd = int.from_bytes(dbr[0x0E:0x10], "little")
        nfats = dbr[0x10]
        fatsz = int.from_bytes(dbr[0x24:0x28], "little")
        root = int.from_bytes(dbr[0x2C:0x30], "little")
        fat_off = (part + rsvd) * bps
        data_off = (part + rsvd + nfats * fatsz) * bps

        def r(o, n):
            f.seek(o)
            return f.read(n)

        def w(o, d):
            f.seek(o)
            f.write(d)

        def fg(c):
            return struct.unpack("<I", r(fat_off + c * 4, 4))[0] & 0x0FFFFFFF

        def fs_(c, v):
            ent = struct.unpack("<I", r(fat_off + c * 4, 4))[0] & 0xF0000000
            w(fat_off + c * 4, struct.pack("<I", ent | (v & 0x0FFFFFFF)))
            w(fat_off + fatsz * bps + c * 4,
              struct.pack("<I", ent | (v & 0x0FFFFFFF)))

        def clo(c):
            return data_off + (c - 2) * spc * bps

        def chain(c):
            out, seen = [], set()
            while 2 <= c < 0x0FFFFFF8 and c not in seen:
                seen.add(c)
                out.append(c)
                c = fg(c)
            return out

        NAME = b"VIDEOMOD   "
        etc = None
        for c in chain(root):
            buf = r(clo(c), spc * bps)
            for i in range(0, spc * bps, 32):
                e = buf[i:i + 32]
                if e[0:11] == b"ETC        ":
                    etc = (struct.unpack("<H", e[20:22])[0] << 16) | \
                          struct.unpack("<H", e[26:28])[0]
        if etc is None:
            raise RuntimeError("no /ETC directory on the image")

        slot, old = None, None
        for c in chain(etc):
            buf = r(clo(c), spc * bps)
            for i in range(0, spc * bps, 32):
                e = buf[i:i + 32]
                if e[0] == 0:
                    slot = clo(c) + i
                    break
                if e[0] == 0xE5:
                    slot = clo(c) + i
                    continue
                if e[0:11] == NAME:
                    slot = clo(c) + i
                    old = (struct.unpack("<H", e[20:22])[0] << 16) | \
                          struct.unpack("<H", e[26:28])[0]
            if slot is not None:
                break
        if slot is None:
            raise RuntimeError("/ETC directory full")

        need = max(1, (len(content) + spc * bps - 1) // (spc * bps))
        free, cl = [], 2
        while len(free) < need:
            if fg(cl) == 0:
                free.append(cl)
            cl += 1
        for i, c in enumerate(free):
            w(clo(c), content[i * spc * bps:(i + 1) * spc * bps]
              .ljust(spc * bps, b"\0"))
            fs_(c, 0x0FFFFFFF if i == need - 1 else free[i + 1])
        if old:
            for c in chain(old):
                fs_(c, 0)

        ent = bytearray(32)
        ent[0:11] = NAME
        ent[11] = 0x20
        ent[20:22] = struct.pack("<H", (free[0] >> 16) & 0xFFFF)
        ent[26:28] = struct.pack("<H", free[0] & 0xFFFF)
        ent[28:32] = struct.pack("<I", len(content))
        w(slot, bytes(ent))


# --------------------------------------------------------------------------
# back-end identity read through the monitor
# --------------------------------------------------------------------------
def _symbols():
    out = subprocess.run(["nm", PE], capture_output=True, text=True).stdout
    table = {}
    for line in out.splitlines():
        parts = line.split()
        if parts and parts[-1] in ("_be", "_be_text", "_be_gfx",
                                   "_be_vbe", "_be_vbe24"):
            table[parts[-1]] = int(parts[0], 16)
    return table


def _xp(s, addr, n=1):
    out = cmd(s, "xp /%dxw 0x%x" % (n, addr), 1.0)
    return [int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{8})", out)]


def read_mode(s):
    """Return dict(be, name, mode, pw, ph, bpp) or None on probe failure."""
    sym = _symbols()
    if "_be" not in sym:
        return None
    ptr = _xp(s, sym["_be"], 1)
    if not ptr:
        return None
    b = ptr[0] & 0xFFFFFFFF
    words = _xp(s, b, 7)
    if len(words) < 7:
        return None
    pw = words[2] & 0xFFFF
    ph = (words[2] >> 16) & 0xFFFF
    bpp = words[4] & 0xFF
    mode = words[6] & 0xFF
    name = {sym.get("_be_text"): "text", sym.get("_be_gfx"): "gfx",
            sym.get("_be_vbe"): "vbe", sym.get("_be_vbe24"): "vbe24"} \
        .get(b, "?")
    return dict(be=b, name=name, mode=mode, pw=pw, ph=ph, bpp=bpp)


def ppm_size(path):
    if not os.path.exists(path):
        return None
    dd = open(path, "rb").read()
    m = re.match(rb"P6\s+(\d+)\s+(\d+)", dd)
    return (int(m.group(1)), int(m.group(2))) if m else None


def boot_case(qemu, iso, tag, content, vga, port):
    """Stage a private disk copy, optionally inject /etc/videomode, boot."""
    work = tempfile.mkdtemp(prefix="vm256_vm_")
    disk = os.path.join(work, "disk.img")
    shutil.copy2(SHARED_DISK, disk)
    if content is not None:
        write_videomode(disk, content)
    proc, s = boot(qemu, iso, port, disk=disk,
                   extra_args=["-vga", vga])
    # PS/2 ghost keys: drain stale input, let the shell settle.
    time.sleep(2)
    time.sleep(14)
    return proc, s, work


def main(argv):
    iso = ISO
    if "--qemu" in argv:
        qemu = find_qemu(argv[argv.index("--qemu") + 1])
    else:
        qemu = find_qemu(None)
    if not qemu:
        print("qemu-system-i386 not found (pass --qemu PATH)")
        return 2
    for a in argv:
        if a.endswith(".iso"):
            iso = os.path.abspath(a)
    if not os.path.isfile(iso):
        print("ISO not found: %s" % iso)
        return 2

    results = []

    def check(name, ok, note=""):
        results.append((name, ok))
        print("%-4s %-32s %s" % ("PASS" if ok else "FAIL", name, note))

    port = PORT

    # a) no file -> default gfx24, 800x600x24
    try:
        proc, s, work = boot_case(qemu, iso, "a", None, "std", port)
        try:
            # exercise the `display` built-in; in a framebuffer mode its text
            # lands on the LFB, so the mode/geometry assertion is read from
            # the back-end struct through the monitor instead of 0xB8000.
            type_text(s, "display")
            sendkey(s, "ret", 1.5)
            m = read_mode(s)
            shot = os.path.join(work, "a.ppm")
            cmd(s, "screendump " + shot, 1.0)
            time.sleep(1.2)
            size = ppm_size(shot)
            ok = (m and m["mode"] == 3 and m["pw"] == 800 and m["ph"] == 600
                  and m["bpp"] == 24 and size == (800, 600))
            check("a: default gfx24 800x600x24", ok,
                  "mode=%s geom=%dx%d bpp=%d ppm=%s" %
                  (m and m["mode"], m and m["pw"], m and m["ph"],
                   m and m["bpp"], size))
        finally:
            kill_qemu(proc)
            shutil.rmtree(work, ignore_errors=True)
    except Exception as e:
        check("a: default gfx24 800x600x24", False, "exception: %s" % e)

    # b) text -> text mode, prompt on the legacy buffer, no warning
    port += 1
    try:
        proc, s, work = boot_case(qemu, iso, "b", b"text\n", "std", port)
        try:
            m = read_mode(s)
            t = screen_text(s)
            ok = m and m["mode"] == 0 and ("catos:" in t) and \
                "WARNING" not in t
            check("b: text config boots text", ok,
                  "mode=%s prompt=%s warn=%s" %
                  (m and m["mode"], "catos:" in t, "WARNING" in t))
        finally:
            kill_qemu(proc)
            shutil.rmtree(work, ignore_errors=True)
    except Exception as e:
        check("b: text config boots text", False, "exception: %s" % e)

    # c) gfx / vbe -> requested mode, or a safe text fallback
    for tag, content, want in (("c1", b"gfx\n", 1), ("c2", b"vbe\n", 2)):
        port += 1
        try:
            proc, s, work = boot_case(qemu, iso, tag, content, "std", port)
            try:
                m = read_mode(s)
                ok = m and (m["mode"] == want or m["mode"] == 0)
                check("c: %s config -> %s or fallback" % (tag, MODE_NAME[want]),
                      ok, "mode=%s geom=%dx%d bpp=%d" %
                      (m and m["mode"], m and m["pw"], m and m["ph"],
                       m and m["bpp"]))
            finally:
                kill_qemu(proc)
                shutil.rmtree(work, ignore_errors=True)
        except Exception as e:
            check("c: %s" % tag, False, "exception: %s" % e)

    # d) malformed content -> default gfx24, no crash
    junk = {
        "d1-long": b"a" * 28 + b"\n",
        "d2-binary": bytes([0xFF, 0x00, 0x01, 0x80, 0x55]),
        "d3-blank": b"   \n\n  \n",
        "d4-junk": b"turbo3d\n",
    }
    for tag, content in junk.items():
        port += 1
        try:
            proc, s, work = boot_case(qemu, iso, tag, content, "std", port)
            try:
                m = read_mode(s)
                ok = m and m["mode"] == 3
                check("d: %s -> default gfx24" % tag, ok,
                      "mode=%s geom=%dx%d bpp=%d" %
                      (m and m["mode"], m and m["pw"], m and m["ph"],
                       m and m["bpp"]))
            finally:
                kill_qemu(proc)
                shutil.rmtree(work, ignore_errors=True)
        except Exception as e:
            check("d: %s" % tag, False, "exception: %s" % e)

    # e) no VBE (-vga cirrus) -> text fallback with a reason on screen
    port += 1
    try:
        proc, s, work = boot_case(qemu, iso, "e", None, "cirrus", port)
        try:
            m = read_mode(s)
            t = screen_text(s)
            warn = ("WARNING" in t) and ("fallback" in t)
            ok = m and m["mode"] == 0 and warn
            check("e: cirrus falls back to text with reason", ok,
                  "mode=%s warn=%s" % (m and m["mode"], warn))
        finally:
            kill_qemu(proc)
            shutil.rmtree(work, ignore_errors=True)
    except Exception as e:
        check("e: cirrus falls back to text with reason", False,
              "exception: %s" % e)

    total = len(results)
    okn = sum(1 for _, ok in results if ok)
    print("\nvideomode_regress: %d/%d PASS" % (okn, total))
    failed = [n for n, ok in results if not ok]
    if failed:
        print("FAILED: " + ", ".join(failed))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
