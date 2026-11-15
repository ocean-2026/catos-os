#!/usr/bin/env python3
"""mkpart_disk.py - build a raw IDE disk with an MBR and TWO FAT32
partitions, for the CatOS P2 mount/umount/df on-metal regression.

Layout (LBA sectors, 512 B):
  LBA 0          MBR (two 0x0C FAT32 entries)
  P1             start 2048, ~14 MiB  (boot/root volume; carries an empty
                                      "docs" mountpoint directory)
  P2             start 30720, ~16 MiB (secondary volume; carries note.txt)

The on-disk FAT32 layout mirrors kernel fat_mkfs() / build/mkdisk.py:
RESERVED=32, NUM_FATS=2, ROOT_CLUS=2, spc=8, two mirrored FATs.

usage:
  python mkpart_disk.py -o qemu_disk_part.img
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "kernel_v25", "build"))
from mkdisk import name83, dent, dots_dents  # noqa: E402

RESERVED = 32
NUM_FATS = 2
ROOT_CLUS = 2
FAT32_EOC = 0x0FFFFFFF
DOS_DATE = ((2026 - 1980) << 9) | (1 << 5) | 1
DOS_TIME = 0

STD_DIRS = ["dev", "etc", "bin", "sbin", "lib", "usr",
            "home", "root", "tmp", "var", "proc", "sys",
            "mnt", "boot", "appdevelop"]


class Volume(object):
    def __init__(self, disk, start, part_sec, label):
        self.disk = disk
        self.start = start
        self.part_sec = part_sec
        self.label = label
        self.spc = 8
        # fatsz iteration, identical to fat_mkfs()
        fatsz = 1
        while True:
            data_sec = part_sec - RESERVED - NUM_FATS * fatsz
            nclus = data_sec // self.spc
            need = ((nclus + 2) * 4 + 511) // 512
            if need <= fatsz:
                break
            fatsz = need
        self.fat_sz = fatsz
        self.data_start = start + RESERVED + NUM_FATS * fatsz
        self.next_free = 3
        self.max_clus = fatsz * 512 // 4
        self.fat = bytearray(fatsz * 512)
        struct.pack_into("<I", self.fat, 0, 0x0FFFFFF8)
        struct.pack_into("<I", self.fat, 4, FAT32_EOC)
        struct.pack_into("<I", self.fat, 8, FAT32_EOC)  # root = EOC

    def clus_off(self, clus):
        return (self.data_start + (clus - 2) * self.spc) * 512

    def alloc(self):
        c = self.next_free
        if c >= self.max_clus:
            raise RuntimeError("volume too small: raise partition size")
        struct.pack_into("<I", self.fat, c * 4, FAT32_EOC)
        self.next_free = c + 1
        return c

    def write_clus(self, clus, buf):
        off = self.clus_off(clus)
        self.disk[off:off + len(buf)] = buf

    def dir_add(self, dir_clus, name, attr, clus, size):
        off = self.clus_off(dir_clus)
        csz = self.spc * 512
        region = self.disk[off:off + csz]
        for i in range(0, csz, 32):
            if region[i] == 0x00 or region[i] == 0xE5:
                self.disk[off + i:off + i + 32] = \
                    dent(name83(name), attr, clus, size)
                return
        raise RuntimeError("directory cluster full (%s)" % name)

    def mkdir(self, parent_clus, name):
        nc = self.alloc()
        buf = bytearray(self.spc * 512)
        buf[0:64] = dots_dents(nc)
        self.write_clus(nc, bytes(buf))
        self.dir_add(parent_clus, name, 0x10, nc, 0)
        return nc

    def write_file(self, data):
        csz = self.spc * 512
        first = prev = None
        for i in range(0, max(len(data), 1), csz):
            c = self.alloc()
            if first is None:
                first = c
            else:
                struct.pack_into("<I", self.fat, prev * 4, c)
            prev = c
            chunk = data[i:i + csz]
            self.write_clus(c, chunk.ljust(csz, b"\x00"))
        return first

    def write_metadata(self):
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
        struct.pack_into("<I", sec, 0x1C, self.start)   # hidden sectors
        struct.pack_into("<I", sec, 0x20, self.part_sec)
        struct.pack_into("<I", sec, 0x24, self.fat_sz)
        struct.pack_into("<I", sec, 0x2C, ROOT_CLUS)
        struct.pack_into("<H", sec, 0x30, 2)
        struct.pack_into("<H", sec, 0x32, 6)
        sec[0x40] = 0x80
        sec[0x42] = 0x29
        struct.pack_into("<I", sec, 0x43, 0x78563412)
        sec[0x47:0x52] = self.label.encode()[:11].ljust(11)
        sec[0x52:0x5A] = b"FAT32   "
        sec[510] = 0x55
        sec[511] = 0xAA
        self.wsec(self.start, bytes(sec))
        self.wsec(self.start + 6, bytes(sec))

        # FSInfo
        fs = bytearray(512)
        fs[0:4] = b"RRaA"
        fs[0x1E4:0x1E8] = b"rrAa"
        self.wsec(self.start + 1, bytes(fs))

        # FAT #1 + #2 mirrored
        self.wsec(self.start + RESERVED, bytes(self.fat))
        self.wsec(self.start + RESERVED + self.fat_sz, bytes(self.fat))

    def wsec(self, lba, data):
        off = lba * 512
        self.disk[off:off + len(data)] = data

    def finalize(self):
        # re-write FATs after allocation mutated self.fat
        self.wsec(self.start + RESERVED, bytes(self.fat))
        self.wsec(self.start + RESERVED + self.fat_sz, bytes(self.fat))


def build(out_path):
    total_sec = 32 * 2048                      # 32 MiB
    p1_start, p1_sec = 2048, 28672
    p2_start, p2_sec = 30720, 32768
    disk = bytearray(total_sec * 512)

    # P1: root volume, standard dirs + empty "docs" mountpoint
    v1 = Volume(disk, p1_start, p1_sec, "CATOSVOL  ")
    v1.write_metadata()
    root = ROOT_CLUS
    rbuf = bytearray(v1.spc * 512)
    rbuf[0:64] = dots_dents(root)
    v1.write_clus(root, bytes(rbuf))
    dirs = {}
    for d in STD_DIRS:
        dirs[d] = v1.mkdir(root, d)
    v1.mkdir(root, "docs")                     # empty mountpoint target
    v1.finalize()

    # P2: secondary volume with one pre-existing marker file
    v2 = Volume(disk, p2_start, p2_sec, "CATOSVOL2 ")
    v2.write_metadata()
    rbuf2 = bytearray(v2.spc * 512)
    rbuf2[0:64] = dots_dents(ROOT_CLUS)
    v2.write_clus(ROOT_CLUS, bytes(rbuf2))
    note = b"hello from partition 2\n"
    fc = v2.write_file(note)
    v2.dir_add(ROOT_CLUS, "note.txt", 0x20, fc, len(note))
    v2.finalize()

    # MBR: two FAT32 LBA entries (0x0C)
    mbr = bytearray(512)
    for i, (start, size) in enumerate(((p1_start, p1_sec),
                                        (p2_start, p2_sec))):
        pe = 446 + i * 16
        mbr[pe + 4] = 0x0C
        struct.pack_into("<I", mbr, pe + 8, start)
        struct.pack_into("<I", mbr, pe + 12, size)
    mbr[510] = 0x55
    mbr[511] = 0xAA
    disk[0:512] = mbr

    with open(out_path, "wb") as fh:
        fh.write(disk)
    print("OK -> %s (32 MiB, P1@%d/%d, P2@%d/%d, spc=%d, fatsz=%d/%d)"
          % (out_path, p1_start, p1_sec, p2_start, p2_sec,
             v1.spc, v1.fat_sz, v2.fat_sz))


def main():
    out = "qemu_disk_part.img"
    args = sys.argv[1:]
    if "-o" in args:
        out = args[args.index("-o") + 1]
    build(out)


if __name__ == "__main__":
    main()
