#!/usr/bin/env python3
"""fs_badname_regress.py -- v2.55 bad-directory-entry purification regression.

Four classes of directory entry are injected into an OFFLINE copy of the
boot volume (QEMU never sees the pristine image):

  1. spec-legal 0x05 first byte  (FAT escape for a real 0xE5 character) --
     must stay visible and must NOT be flagged by fsck;
  2. 8.3 name carrying a control byte (0x01) -- hidden by default;
  3. stale LFN run whose checksum does not match the short entry -- the
     short entry keeps a legal name and stays visible, but the run is
     reported as orphan-lfn;
  4. 8.3 name carrying a FAT reserved character ('?') -- hidden by default.

Assertions:
  a) default ls hides the illegal entries and prints the hidden-count
     summary; normal entries and the 0x05 escape are unaffected;
  b) ls --raw lists every live entry with c=<cluster> o=<offset>
     first=<hex> raw=<hex bytes>;
  c) fsck reports the right problem kinds / counts, nothing auto-deleted;
  d) fsck --purge removes only the repairable bad entries; a second fsck
     reports a clean tree; no fatal fault.

usage:
  python tests/fs_badname_regress.py <catos.iso> [--qemu PATH] [--port N]
"""
import os
import shutil
import struct
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qemu_driver import (boot, find_qemu, kill_qemu, screen_text, sendkey,
                         type_text, wait_for_prompt, force_text)

PORT = 4551


class R(object):
    def __init__(self):
        self.n = 0
        self.fails = []

    def check(self, name, cond, detail=""):
        self.n += 1
        ok = bool(cond)
        print("  %-34s %s" % (name, "PASS" if ok else "FAIL"))
        if not ok:
            self.fails.append(name)
            if detail:
                print("      " + detail[-240:].replace("\n", " "))


def drain_pager(s, wait=0.6, max_steps=12):
    """v2.59: long output is paged; page through until the More bar is gone
    so the tail (count line / last entries) is really on screen."""
    for _ in range(max_steps):
        if "-- more" not in screen_text(s).lower():
            return
        sendkey(s, "spc", wait)


def run(s, c, wait=1.3):
    type_text(s, c)
    sendkey(s, "ret")
    time.sleep(wait)
    drain_pager(s)
    return screen_text(s)


def clean_run(s, c, wait=1.5):
    type_text(s, "clear")
    sendkey(s, "ret")
    time.sleep(0.8)
    return run(s, c, wait)


# ---------- offline FAT geometry + injection ---------------------------

def parse_geometry(img):
    with open(img, "rb") as f:
        mbr = f.read(512)
    pe = 446
    part_start = int.from_bytes(mbr[pe + 8:pe + 12], "little")
    with open(img, "rb") as f:
        f.seek(part_start * 512)
        dbr = f.read(512)
    bps = int.from_bytes(dbr[0x0B:0x0D], "little")
    spc = dbr[0x0D]
    rsvd = int.from_bytes(dbr[0x0E:0x10], "little")
    nfats = dbr[0x10]
    fatsz = int.from_bytes(dbr[0x24:0x28], "little")
    root = int.from_bytes(dbr[0x2C:0x30], "little")
    fat_off = (part_start + rsvd) * bps
    data_off = (part_start + rsvd + nfats * fatsz) * bps
    return dict(bps=bps, spc=spc, rsvd=rsvd, nfats=nfats, fatsz=fatsz,
                root=root, fat_off=fat_off, data_off=data_off)


def read_at(img, off, n):
    with open(img, "rb") as f:
        f.seek(off)
        return f.read(n)


def write_at(img, off, data):
    with open(img, "r+b") as f:
        f.seek(off)
        f.write(data)


def fat_get(G, cl):
    off = G["fat_off"] + cl * 4
    return struct.unpack("<I", read_at(img_holder[0], off, 4))[0] & 0x0FFFFFFF


def fat_set(G, cl, val):
    ent_off = G["fat_off"] + cl * 4
    ent = struct.unpack("<I", read_at(img_holder[0], ent_off, 4))[0] & 0xF0000000
    blob = struct.pack("<I", ent | (val & 0x0FFFFFFF))
    write_at(img_holder[0], ent_off, blob)
    write_at(img_holder[0], ent_off + G["fatsz"] * G["bps"], blob)


def cluster_off(G, cl):
    return G["data_off"] + (cl - 2) * G["spc"] * G["bps"]


def dent(name11, attr, clus, size=0):
    e = bytearray(32)
    e[0:11] = name11
    e[11] = attr
    e[20:22] = struct.pack("<H", (clus >> 16) & 0xFFFF)
    e[26:28] = struct.pack("<H", clus & 0xFFFF)
    e[28:32] = struct.pack("<I", size)
    return bytes(e)


def lfn_stale():
    """One 0x0F entry with a deliberately wrong checksum (byte 13 = 0x00)."""
    b = bytearray(32)
    b[0] = 0x41
    b[11] = 0x0F
    b[12] = 0
    b[13] = 0x00
    slots = [1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30]
    for k in slots:
        b[k] = 0x58      # 'X' UCS-2 low byte
        b[k + 1] = 0x00
    return bytes(b)


# current image path, shared with fat_get/fat_set without threading it
# through every helper argument
img_holder = [None]


def patch_image(src, dst):
    shutil.copy2(src, dst)
    img_holder[0] = dst
    G = parse_geometry(dst)
    csz = G["spc"] * G["bps"]

    free = None
    cl = 2
    while cl < 0x0FFFFFF0:
        if fat_get(G, cl) == 0:
            free = cl
            break
        cl += 1
    if free is None:
        raise RuntimeError("no free cluster for injected directory")

    # 0x05 first byte: spec escape for a real 0xE5 character -- sane.
    legal = dent(b"\x05Y" + b" " * 6 + b"TXT", 0x20, G["root"])
    # control byte 0x01 in the base name -- insane.
    ctrl = dent(b"A\x01B" + b" " * 5 + b"TXT", 0x20, G["root"])
    # reserved character '?' in the base name -- insane.
    resv = dent(b"A?B" + b" " * 5 + b"TXT", 0x20, G["root"])
    stale_lfn = lfn_stale()
    stale = dent(b"STALE" + b" " * 3 + b"TXT", 0x20, G["root"])
    # two ordinary entries so the directory also proves "normal items are
    # unaffected" (the seed image is regenerated by build.sh, so the test
    # must not rely on them already existing).
    keep1 = dent(b"KEEP" + b" " * 5 + b"TXT", 0x20, G["root"])
    keep2 = dent(b"NOTE" + b" " * 5 + b"TXT", 0x20, G["root"])

    buf = bytearray(csz)
    slots = [
        dent(b"." + b" " * 10, 0x10, free),
        dent(b".." + b" " * 9, 0x10, G["root"]),
        legal, ctrl, resv, stale_lfn, stale, keep1, keep2,
    ]
    for i, e in enumerate(slots):
        buf[i * 32:i * 32 + 32] = e
    write_at(dst, cluster_off(G, free), bytes(buf))
    fat_set(G, free, 0x0FFFFFFF)

    root_buf = read_at(dst, cluster_off(G, G["root"]), csz)
    slot = None
    for i in range(0, csz, 32):
        if root_buf[i] == 0 or root_buf[i] == 0xE5:
            slot = i
            break
    if slot is None:
        raise RuntimeError("root directory full")
    write_at(dst, cluster_off(G, G["root"]) + slot,
             dent(b"BADTEST" + b" " * 4, 0x10, free))
    return G, free


def main():
    iso = os.path.abspath(sys.argv[1])
    qemu = find_qemu(sys.argv[sys.argv.index("--qemu") + 1]
                     if "--qemu" in sys.argv else None)
    port = PORT
    if "--port" in sys.argv:
        i = sys.argv.index("--port")
        port = int(sys.argv[i + 1])

    src_disk = os.path.abspath(os.path.join(
        os.path.dirname(HERE), "kernel_v25", "catos_disk.img"))
    tmpdir = tempfile.mkdtemp(prefix="catos_badname_")
    dst_disk = os.path.join(tmpdir, "badname.img")
    patch_image(src_disk, dst_disk)

    proc, sock = boot(qemu, iso, port, disk=dst_disk)
    r = R()
    try:
        force_text(sock)
        wait_for_prompt(sock)
        type_text(sock, "clear")
        sendkey(sock, "ret")
        time.sleep(1.5)

        # a) default ls: illegal entries hidden, summary printed.
        #    Visible: . .. + spec-legal 0x05 escape + stale short entry
        #    (its name is legal; only the orphan LFN is flagged by fsck).
        #    Hidden: control-byte entry + reserved-char entry = 2.
        t = clean_run(sock, "ls /badtest")
        r.check("default-hides-reserved", "a?b.txt" not in t, t)
        r.check("default-shows-stale", "stale.txt" in t, t)
        r.check("hidden-count-summary", "2 hidden invalid entries" in t, t)
        r.check("legal-escape-not-hidden", "4 entries" in t, t)

        # b) ls --raw: every live entry with c/o/first/raw detail
        t = clean_run(sock, "ls --raw /badtest")
        r.check("raw-shows-cluster", "c=2" in t, t)
        r.check("raw-shows-offset", "o=" in t, t)
        r.check("raw-shows-first", "first=" in t, t)
        r.check("raw-shows-rawbytes", "raw=" in t, t)
        r.check("raw-shows-reserved", "a?b.txt" in t, t)
        r.check("raw-shows-stale", "stale.txt" in t, t)
        r.check("raw-shows-six-entries", "6 entries" in t, t)

        # c) fsck report: 3 bad, 3 repairable, 0 rebuild, 2 bad-name + 1 orphan
        t = clean_run(sock, "fsck")
        r.check("fsck-reports-three", "3 bad entries" in t, t)
        r.check("fsck-all-repairable", "3 repairable" in t, t)
        r.check("fsck-no-rebuild", "0 need rebuild" in t, t)
        r.check("fsck-badname-x2", t.count("kind=bad-name") >= 2, t)
        r.check("fsck-orphan-lfn", "kind=orphan-lfn" in t, t)
        # the spec-legal 0x05 escape must not be reported
        r.check("fsck-legal-untouched", "first=0x05" not in t, t)

        # d) normal entries on the volume are unaffected (tail of the
        #    alphabetically sorted /bin listing stays on screen)
        t = clean_run(sock, "ls /bin")
        r.check("normal-bin-intact", "zhdemo.ce" in t, t)

        # e) explicit purge only: bad entries gone, legal + dots remain
        t = clean_run(sock, "fsck --purge")
        r.check("purge-acknowledged", "purged" in t, t)
        t = clean_run(sock, "ls --raw /badtest")
        r.check("purge-removed-reserved", "a?b.txt" not in t, t)
        r.check("purge-removed-stale", "stale.txt" not in t, t)
        t = clean_run(sock, "fsck")
        r.check("fsck-clean-after-purge", "directory tree OK" in t, t)

        t = clean_run(sock, "faultinfo")
        r.check("no-fatal-fault",
                "panic" not in t.lower() and "fatal" not in t.lower(), t)
    finally:
        kill_qemu(proc)

    print("fs_badname_regress: %d/%d PASS%s" % (
        r.n - len(r.fails), r.n,
        "" if not r.fails else "  FAILED: " + ", ".join(r.fails)))
    return 0 if not r.fails else 1


if __name__ == "__main__":
    sys.exit(main())
