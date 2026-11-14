#!/usr/bin/env python3
"""mkdisk.py - build a FAT32 disk image with /bin pre-populated.

This is the host-side twin of kernel fat_mkfs() (src/fat.c).  It
replicates the EXACT on-disk layout the kernel produces -- MBR with one
FAT32 partition (type 0x0C) at LBA 1, RESERVED=32, NUM_FATS=2,
ROOT_CLUS=2, the same spc/fatsz iteration, the same standard directory
list -- so CatOS mounts the image without running `format`.

Why: the kernel image embeds the CE apps only as a *fallback*.  With a
pre-populated disk, ce_install_builtins() finds every file already on
the volume and installs nothing, so apps on the disk are the real
source of truth and the kernel copy is just a bootstrap.  That decouples
the application set from kernel builds without any GRUB tooling.

File list comes from gen_builtin.py (single source of truth shared with
build.sh and the kernel image), so an image always matches what the
kernel would install.

usage:
  python mkdisk.py -o catos_disk.img [-m 64] [--no-files]
  python mkdisk.py --verify catos_disk.img   (self-check only)
"""
import importlib.util
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)          # kernel_v25/

# --- layout constants: MUST match src/fat.c fat_mkfs() ---
PART_START = 1                        # partition starts right after MBR
RESERVED = 32
NUM_FATS = 2
ROOT_CLUS = 2
FAT_CACHE_MAX = 4 * 1024 * 1024       # same cap as fat.c
FAT32_EOC = 0x0FFFFFFF

STD_DIRS = ["dev", "etc", "bin", "sbin", "lib", "usr",
            "home", "root", "tmp", "var", "proc", "sys",
            "mnt", "boot", "appdevelop"]

# fixed FAT timestamp: 2026-01-01 00:00
DOS_DATE = ((2026 - 1980) << 9) | (1 << 5) | 1
DOS_TIME = 0


def _load_gen_builtin():
    """Import gen_builtin.py for the shared file list (no side effects:
    its emit path is guarded by __main__)."""
    spec = importlib.util.spec_from_file_location(
        "gen_builtin", os.path.join(HERE, "gen_builtin.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def name83(name):
    """Same rules as fat_name83(): 8.3 upper-case, no LFN."""
    out = bytearray(b" " * 11)
    dot = name.rfind(".")
    stem = name[:dot] if dot > 0 else name
    ext = name[dot + 1:] if dot > 0 else ""
    for i, ch in enumerate(stem.upper()[:8]):
        out[i] = ord(ch)
    for i, ch in enumerate(ext.upper()[:3]):
        out[8 + i] = ord(ch)
    return bytes(out)


def dent(name83bytes, attr, clus, size):
    """Same field layout as struct dent (standard FAT32)."""
    assert len(name83bytes) == 11, "83-name must be exactly 11 bytes"
    d = bytearray(32)
    d[0:11] = name83bytes
    d[11] = attr
    d[13] = 0                                   # CrtTimeTenth
    struct.pack_into("<H", d, 14, DOS_TIME)     # CrtTime
    struct.pack_into("<H", d, 16, DOS_DATE)     # CrtDate
    struct.pack_into("<H", d, 18, DOS_DATE)     # LstAccDate
    struct.pack_into("<H", d, 20, clus >> 16)   # FstClusHI
    struct.pack_into("<H", d, 22, DOS_TIME)     # WrtTime
    struct.pack_into("<H", d, 24, DOS_DATE)     # WrtDate
    struct.pack_into("<H", d, 26, clus & 0xFFFF)  # FstClusLO
    struct.pack_into("<I", d, 28, size)
    return bytes(d)


def dots_dents(parent_clus):
    """'.' and '..' entries exactly as fat_mkfs/fat_mkdir write them."""
    a = bytearray(b" " * 11)
    b = bytearray(b" " * 11)
    a[0] = ord(".")
    b[0:2] = b".."
    return dent(bytes(a), 0x10, parent_clus, 0) + \
           dent(bytes(b), 0x10, parent_clus, 0)


class FatImage(object):
    def __init__(self, miB):
        self.total_sec = miB * 2048
        self.part_sec = self.total_sec - 1
        # spc selection, identical to fat_mkfs()
        self.spc = 1
        if self.part_sec >= 524288:
            self.spc = 8
        if self.part_sec >= 16777216:
            self.spc = 16
        if self.part_sec >= 33554432:
            self.spc = 32
        if self.part_sec >= 67108864:
            self.spc = 64
        # Kernel fat_mkfs() uses spc=1 below 256 MiB, which gives a
        # 16-entry root cluster -- too small for 15 std dirs + files.
        # fat_probe() reads spc from the DBR, so pick 8 (4 KiB, 128
        # entries/dir) like the FAT32 convention for this size class.
        if self.spc < 8:
            self.spc = 8
        # fatsz iteration, identical to fat_mkfs()
        for _ in range(32):
            fatsz = 1
            while True:
                data_sec = self.part_sec - RESERVED - NUM_FATS * fatsz
                nclus = data_sec // self.spc
                need = ((nclus + 2) * 4 + 511) // 512
                if need <= fatsz:
                    break
                fatsz = need
            if fatsz * 512 <= FAT_CACHE_MAX:
                self.fat_sz = fatsz
                break
            self.spc *= 2
        self.data_start = PART_START + RESERVED + NUM_FATS * self.fat_sz
        self.next_free = 3
        self.max_clus = self.fat_sz * 512 // 4
        # whole image in memory; 64 MiB is the default and plenty
        self.img = bytearray(self.total_sec * 512)
        # FAT entries 0..2, identical to fat_mkfs()
        self.fat = bytearray(self.fat_sz * 512)
        struct.pack_into("<I", self.fat, 0, 0x0FFFFFF8)
        struct.pack_into("<I", self.fat, 4, FAT32_EOC)
        struct.pack_into("<I", self.fat, 8, FAT32_EOC)   # root = EOC

    # ---- low-level helpers ----
    def wsec(self, lba, data):
        assert len(data) % 512 == 0
        off = lba * 512
        self.img[off:off + len(data)] = data

    def clus_off(self, clus):
        return (self.data_start + (clus - 2) * self.spc) * 512

    def fat_get(self, c):
        return struct.unpack_from("<I", self.fat, c * 4)[0]

    def fat_set(self, c, v):
        struct.pack_into("<I", self.fat, c * 4, v)

    def alloc(self):
        c = self.next_free
        if c >= self.max_clus:
            raise RuntimeError("image too small for all files: raise -m MiB")
        self.fat_set(c, FAT32_EOC)
        self.next_free = c + 1
        return c

    def write_clus(self, clus, buf):
        assert len(buf) == self.spc * 512
        off = self.clus_off(clus)
        self.img[off:off + len(buf)] = buf

    def write_file(self, data):
        """Allocate a chain and write data; returns first cluster."""
        csz = self.spc * 512
        first = prev = None
        for i in range(0, max(len(data), 1), csz):
            c = self.alloc()
            if first is None:
                first = c
            else:
                self.fat_set(prev, c)
            prev = c
            chunk = data[i:i + csz]
            self.write_clus(c, chunk.ljust(csz, b"\x00"))
        return first

    # ---- directory helpers ----
    def dir_add(self, dir_clus, name, attr, clus, size):
        """Append one 32-byte dirent into the first free slot of a
        single-cluster directory (all our dirs fit one cluster)."""
        off = self.clus_off(dir_clus)
        csz = self.spc * 512
        region = self.img[off:off + csz]
        for i in range(0, csz, 32):
            if region[i] == 0x00 or region[i] == 0xE5:
                self.img[off + i:off + i + 32] = \
                    dent(name83(name), attr, clus, size)
                return
        raise RuntimeError("directory cluster full (clus %d)" % dir_clus)

    def mkdir(self, parent_clus, name):
        nc = self.alloc()
        buf = bytearray(self.spc * 512)
        buf[0:64] = dots_dents(nc)
        self.write_clus(nc, bytes(buf))
        self.dir_add(parent_clus, name, 0x10, nc, 0)
        return nc

    # ---- metadata sectors: byte-for-byte fat_mkfs() layout ----
    def write_metadata(self):
        # 1) MBR, one FAT32 partition (0x0C) at LBA 1
        mbr = bytearray(512)
        pe = 446
        mbr[pe + 4] = 0x0C
        struct.pack_into("<I", mbr, pe + 8, PART_START)
        struct.pack_into("<I", mbr, pe + 12, self.part_sec)
        mbr[510] = 0x55
        mbr[511] = 0xAA
        self.wsec(0, bytes(mbr))

        # 2) DBR / BPB
        sec = bytearray(512)
        sec[0:3] = b"\xEB\x58\x90"
        sec[3:11] = b"CATOS   "
        struct.pack_into("<H", sec, 0x0B, 512)
        sec[0x0D] = self.spc
        struct.pack_into("<H", sec, 0x0E, RESERVED)
        sec[0x10] = NUM_FATS
        sec[0x15] = 0xF8
        struct.pack_into("<H", sec, 0x18, 63)
        struct.pack_into("<H", sec, 0x1A, 255)
        struct.pack_into("<I", sec, 0x1C, PART_START)   # hidden sectors
        struct.pack_into("<I", sec, 0x20, self.part_sec)
        struct.pack_into("<I", sec, 0x24, self.fat_sz)
        struct.pack_into("<I", sec, 0x2C, ROOT_CLUS)
        struct.pack_into("<H", sec, 0x30, 2)            # FSInfo sector
        struct.pack_into("<H", sec, 0x32, 6)            # backup boot
        sec[0x40] = 0x80
        sec[0x42] = 0x29
        struct.pack_into("<I", sec, 0x43, 0x78563412)   # serial
        sec[0x47:0x52] = b"CATOSVOL   "[:11]
        sec[0x52:0x5A] = b"FAT32   "
        sec[510] = 0x55
        sec[511] = 0xAA
        self.wsec(PART_START, bytes(sec))               # DBR at LBA 1
        self.wsec(PART_START + 6, bytes(sec))           # backup at LBA 7

        # 3) FSInfo (filled after allocation for free counts)
        self.fsinfo = bytearray(512)
        self.fsinfo[0:4] = b"RRaA"
        self.fsinfo[0x1E4:0x1E8] = b"rrAa"
        self.wsec(PART_START + 1, bytes(self.fsinfo))

        # 4) FAT #1 + #2 (mirror, verified by fat_verify_mirror at mount)
        self.wsec(PART_START + RESERVED, bytes(self.fat))
        self.wsec(PART_START + RESERVED + self.fat_sz, bytes(self.fat))

    def finalize_fsinfo(self):
        used = self.next_free - 3          # clusters 3.. allocated
        vol_clusters = (self.part_sec - RESERVED - NUM_FATS * self.fat_sz) \
            // self.spc
        free = vol_clusters - used - 1     # -1: root cluster 2
        fs = self.fsinfo
        struct.pack_into("<I", fs, 0x1E8, max(free, 0))
        struct.pack_into("<I", fs, 0x1EC, self.next_free)
        self.wsec(PART_START + 1, bytes(fs))
        # re-write both FATs: alloc() mutated self.fat after write_metadata
        self.wsec(PART_START + RESERVED, bytes(self.fat))
        self.wsec(PART_START + RESERVED + self.fat_sz, bytes(self.fat))

    # ---- verification (host-side fsck) ----
    def verify(self):
        errs = []
        if self.img[510:512] != b"\x55\xAA":
            errs.append("MBR signature")
        pe = 446
        if self.img[pe + 4] not in (0x0B, 0x0C):
            errs.append("MBR partition type")
        if struct.unpack_from("<I", self.img, pe + 8)[0] != PART_START:
            errs.append("MBR partition start")
        if struct.unpack_from("<I", self.img, pe + 12)[0] != self.part_sec:
            errs.append("MBR partition size")
        dbr = self.img[PART_START * 512:(PART_START + 1) * 512]
        if struct.unpack_from("<I", dbr, 0x2C)[0] != ROOT_CLUS:
            errs.append("DBR root cluster")
        f1 = PART_START + RESERVED
        f2 = f1 + self.fat_sz
        if self.img[f1 * 512:(f1 * 512) + 8] == b"\x00" * 8:
            errs.append("FAT #1 head is empty")
        f1 = PART_START + RESERVED
        f2 = f1 + self.fat_sz
        for c in range(0, self.next_free):
            a = struct.unpack_from("<I", self.img, (f1 * 512) + c * 4)[0]
            b = struct.unpack_from("<I", self.img, (f2 * 512) + c * 4)[0]
            if a != b:
                errs.append("FAT mirror mismatch at cluster %d" % c)
                break
        # walk every file chain end-to-end, reading the FAT from the
        # IMAGE bytes (not the in-memory mirror) so a stale/unwritten
        # FAT region is caught
        fat1_off = (PART_START + RESERVED) * 512

        def img_fat(c):
            return struct.unpack_from("<I", self.img, fat1_off + c * 4)[0] \
                & 0x0FFFFFFF

        for path, clus, size in getattr(self, "_file_index", []):
            cur = clus
            remaining = size
            seen = set()
            while True:
                if cur in seen:
                    errs.append("cluster loop in %s" % path)
                    break
                seen.add(cur)
                v = img_fat(cur)
                if remaining <= self.spc * 512:
                    if v != (FAT32_EOC & 0x0FFFFFFF):
                        errs.append("%s: chain not EOC-terminated" % path)
                    break
                if v < 3 or v >= self.max_clus:
                    errs.append("%s: bad next-cluster %d" % (path, v))
                    break
                remaining -= self.spc * 512
                cur = v
        return errs


def build(out_path, miB, with_files=True):
    g = _load_gen_builtin()
    img = FatImage(miB)
    img.write_metadata()

    # root cluster + standard dirs, same order as fat_mkfs()
    root = ROOT_CLUS
    rbuf = bytearray(img.spc * 512)
    rbuf[0:64] = dots_dents(root)
    img.write_clus(root, bytes(rbuf))
    dirs = {}
    for d in STD_DIRS:
        dirs[d] = img.mkdir(root, d)

    # files, single-sourced from gen_builtin.py
    selected = list(g.FILES)
    for path, src in g.OPTIONAL:
        if os.path.exists(src):
            selected.append((path, src))
    img._file_index = []
    if with_files:
        for path, src in selected:
            if not os.path.exists(src):
                print("note: skip %s (not built)" % path, file=sys.stderr)
                continue
            with open(src, "rb") as fh:
                data = fh.read()
            parent = dirs[path.strip("/").split("/")[0]]
            first = img.write_file(data)
            img.dir_add(parent, os.path.basename(path), 0x20, first,
                        len(data))
            img._file_index.append((path, first, len(data)))
    img.finalize_fsinfo()

    with open(out_path, "wb") as fh:
        fh.write(img.img)
    errs = img.verify()
    n_files = len(img._file_index)
    print("OK -> %s (%d MiB, spc=%d, fatsz=%d, %d dirs, %d files, "
          "%d clusters used)" % (out_path, miB, img.spc, img.fat_sz,
                                 len(STD_DIRS), n_files,
                                 img.next_free - 2))
    if errs:
        for e in errs:
            print("VERIFY FAIL: %s" % e, file=sys.stderr)
        return 1
    return 0


def main():
    args = sys.argv[1:]
    out = "catos_disk.img"
    miB = 64
    with_files = True
    i = 0
    while i < len(args):
        a = args[i]
        if a == "-o":
            out = args[i + 1]
            i += 2
        elif a == "-m":
            miB = int(args[i + 1])
            i += 2
        elif a == "--no-files":
            with_files = False
            i += 1
        else:
            print("usage: mkdisk.py -o catos_disk.img [-m 64] [--no-files]")
            return 2
    return build(out, miB, with_files)


if __name__ == "__main__":
    sys.exit(main())
