#!/usr/bin/env python3
"""Inject a file into /BIN/ on a CatOS-formatted FAT32 volume.

Usage: inject_any.py <img> <src-file> <8.3-NAME>   e.g. TEST_AB.CE

P1-7 -- the FAT geometry is now READ FROM THE DISK instead of being
hard-coded.  The old version assumed RSVD=33 / SPC=8 / FAT_SZ=8192 (a 4 GiB
volume), but the kernel's fat_mkfs() picks the sectors-per-cluster and the
FAT size from the drive capacity, and it always writes RESERVED=32.  On a
64 MiB volume the kernel uses SPC=1 / FAT_SZ=1024, so the hard-coded
geometry addressed the wrong sectors and every injection corrupted the
volume or failed outright.

The BPB (BIOS Parameter Block) lives in the DBR, which the kernel writes
to LBA 1 (PART_START = 1) -- the MBR at LBA 0 points there through its
partition entry.  This script therefore:

  1. reads the MBR (LBA 0) and takes the partition's start LBA from it,
  2. reads the DBR at that LBA and takes bytes-per-sector, sectors-per-
     cluster, reserved sectors, FAT count, FAT size and root cluster from
     the BPB,
  3. derives the FAT offset and the data area offset from those values.

Everything else (directory walking, free-cluster search, chain rewriting)
is unchanged.
"""
import struct
import sys

if len(sys.argv) < 4:
    print(__doc__)
    sys.exit(2)

IMG = sys.argv[1]
SRC = sys.argv[2]
NAME83 = sys.argv[3]

# Build a standard 8.3 directory entry (8-char name + 3-char ext, NO dot,
# space padded).  The rules mirror the kernel's fat_name83() in src/fat.c:
# the LAST dot separates base from extension, both are upper-cased, the
# base is truncated to 8 characters and the extension to 3.
def name83_bytes(name):
    dot = name.rfind('.')
    base = name if dot < 0 else name[:dot]
    ext = '' if dot < 0 else name[dot + 1:]
    out = bytearray(b' ' * 11)
    b = base[:8].upper().encode('latin-1', 'replace')
    e = ext[:3].upper().encode('latin-1', 'replace')
    out[0:len(b)] = b
    out[8:8 + len(e)] = e
    return bytes(out)


name_bytes = name83_bytes(NAME83)


def r(off, n):
    with open(IMG, 'rb') as f:
        f.seek(off)
        return f.read(n)


def w(off, data):
    with open(IMG, 'r+b') as f:
        f.seek(off)
        f.write(data)


def read_geometry():
    """Derive the FAT32 layout from the MBR + DBR on the image."""
    mbr = r(0, 512)
    if mbr[510] != 0x55 or mbr[511] != 0xAA:
        raise RuntimeError('no MBR signature at LBA 0 (not a partitioned image?)')

    # The partition table starts at offset 446 (0x1BE).  The first entry
    # is laid out as: boot(1) CHS-start(3) type(1) CHS-end(3)
    # LBA-start(4) num-sectors(4).  The kernel writes the FAT32 partition
    # with type 0x0C at LBA 1 (see fat_mkfs() in src/fat.c).
    PE = 446
    if mbr[PE + 4] == 0:
        raise RuntimeError('no partition entry in the MBR')
    part_start = int.from_bytes(mbr[PE + 8:PE + 12], 'little')
    if part_start == 0:
        raise RuntimeError('partition entry has start LBA 0')

    dbr = r(part_start * 512, 512)
    if dbr[510] != 0x55 or dbr[511] != 0xAA:
        raise RuntimeError('no DBR signature at LBA %d (volume not formatted?)'
                           % part_start)

    bps = int.from_bytes(dbr[0x0B:0x0D], 'little')      # bytes per sector
    spc = dbr[0x0D]                                      # sectors per cluster
    rsvd = int.from_bytes(dbr[0x0E:0x10], 'little')      # reserved sectors
    nfats = dbr[0x10]                                    # number of FATs
    fatsz = int.from_bytes(dbr[0x24:0x28], 'little')     # FAT size (sectors)
    root_clus = int.from_bytes(dbr[0x2C:0x30], 'little') # first cluster of root dir
    total32 = int.from_bytes(dbr[0x20:0x24], 'little')   # total sectors (0 => use size)

    if bps == 0 or spc == 0 or rsvd == 0 or nfats == 0 or fatsz == 0:
        raise RuntimeError('implausible BPB in DBR: bps=%d spc=%d rsvd=%d '
                           'nfats=%d fatsz=%d' % (bps, spc, rsvd, nfats, fatsz))

    fat_off = (part_start + rsvd) * bps
    data_off = (part_start + rsvd + nfats * fatsz) * bps

    # The cluster count is bounded by the FAT itself: each FAT entry is
    # 4 bytes, so the FAT holds (fatsz * bps / 4) clusters.
    total_cl = fatsz * bps // 4

    if total32:
        total_cl = min(total_cl, total32 // spc)

    return dict(part_start=part_start, bps=bps, spc=spc, rsvd=rsvd,
                nfats=nfats, fatsz=fatsz, root_clus=root_clus,
                fat_off=fat_off, data_off=data_off, total_cl=total_cl)


G = read_geometry()
BPS = G['bps']
SPC = G['spc']
NFATS = G['nfats']
FAT_SZ = G['fatsz']
DATA_OFF = G['data_off']
FAT_OFF = G['fat_off']
TOTAL_CL = G['total_cl']
ROOT_CLUS = G['root_clus']


def fat_get(cl):
    return struct.unpack('<I', r(FAT_OFF + cl * 4, 4))[0] & 0x0FFFFFFF


def fat_set(cl, val):
    val &= 0x0FFFFFFF
    ent = struct.unpack('<I', r(FAT_OFF + cl * 4, 4))[0] & 0xF0000000
    w(FAT_OFF + cl * 4, struct.pack('<I', ent | val))
    # the second FAT copy starts right after the first one
    w(FAT_OFF + FAT_SZ * BPS + cl * 4, struct.pack('<I', ent | val))


def cluster_off(cl):
    return DATA_OFF + (cl - 2) * SPC * BPS


def chain(cl):
    out = []
    seen = set()
    while 2 <= cl < 0x0FFFFFF8 and cl not in seen:
        seen.add(cl)
        out.append(cl)
        cl = fat_get(cl)
    return out


def read_chain(cl):
    return b''.join(r(cluster_off(c), SPC * BPS) for c in chain(cl))


def free_run(n):
    free = []
    cl = 2
    while cl < TOTAL_CL and len(free) < n:
        if fat_get(cl) == 0:
            free.append(cl)
        cl += 1
    if len(free) < n:
        raise RuntimeError('not enough free clusters: need %d got %d' % (n, len(free)))
    return free


def find_dir_entries(cl, name_bytes):
    out = []
    for c in chain(cl):
        buf = r(cluster_off(c), SPC * BPS)
        for i in range(0, SPC * BPS, 32):
            e = buf[i:i+32]
            if e[0] == 0:
                return out
            if e[0] == 0xE5:
                continue
            if e[11] == 0x0F:
                continue
            if e[0:11] == name_bytes:
                out.append((cluster_off(c) + i, e[11],
                            (int.from_bytes(e[20:22], 'little') << 16) | int.from_bytes(e[26:28], 'little'),
                            int.from_bytes(e[28:32], 'little')))
    return out


def main():
    print('geometry: part_start=%d bps=%d spc=%d rsvd=%d nfats=%d fatsz=%d '
          'root_clus=%d total_cl=%d'
          % (G['part_start'], BPS, SPC, G['rsvd'], NFATS, FAT_SZ,
             ROOT_CLUS, TOTAL_CL))
    new_data = open(SRC, 'rb').read()
    print('%s: %d bytes' % (NAME83, len(new_data)))
    need = (len(new_data) + SPC * BPS - 1) // (SPC * BPS)
    bin_entries = find_dir_entries(ROOT_CLUS, b'BIN        ')
    if not bin_entries:
        raise RuntimeError('BIN dir not found (disk not formatted?)')
    bin_off, bin_attr, bin_cl, bin_sz = bin_entries[0]

    ots = find_dir_entries(bin_cl, name_bytes)
    if not ots:
        free_slot = None
        for c in chain(bin_cl):
            buf = r(cluster_off(c), SPC * BPS)
            for i in range(0, SPC * BPS, 32):
                if buf[i] == 0 or buf[i] == 0xE5:
                    free_slot = cluster_off(c) + i
                    break
            if free_slot:
                break
        if not free_slot:
            raise RuntimeError('no free dir slot in /BIN')
        nr = free_run(need)
        for i, c in enumerate(nr):
            w(cluster_off(c), new_data[i * SPC * BPS:(i + 1) * SPC * BPS].ljust(SPC * BPS, b'\x00'))
            fat_set(c, 0x0FFFFFFF if i == need - 1 else nr[i + 1])
        ent = bytearray(32)
        ent[0:11] = name_bytes
        ent[11] = 0x20
        ent[12:14] = struct.pack('<H', 0x5241)
        ent[14:16] = struct.pack('<H', 0x8B21)
        ent[16:18] = struct.pack('<H', 0x0000)
        ent[20:22] = struct.pack('<H', (nr[0] >> 16) & 0xFFFF)
        ent[22:24] = struct.pack('<H', 0x0000)
        ent[26:28] = struct.pack('<H', nr[0] & 0xFFFF)
        ent[28:32] = struct.pack('<I', len(new_data))
        w(free_slot, bytes(ent))
        new_start = nr[0]
        print('added: slot 0x%x start cl=%d' % (free_slot, new_start))
    else:
        ot_off, ot_attr, ot_cl, ot_sz = ots[0]
        old_chain = chain(ot_cl)
        if len(old_chain) >= need:
            for i in range(need):
                chunk = new_data[i * SPC * BPS:(i + 1) * SPC * BPS]
                w(cluster_off(old_chain[i]), chunk.ljust(SPC * BPS, b'\x00'))
            if len(old_chain) > need:
                for c in old_chain[need:]:
                    fat_set(c, 0)
                fat_set(old_chain[need - 1], 0x0FFFFFFF)
            new_start = old_chain[0]
        else:
            nr = free_run(need)
            for i, c in enumerate(nr):
                w(cluster_off(c), new_data[i * SPC * BPS:(i + 1) * SPC * BPS].ljust(SPC * BPS, b'\x00'))
                fat_set(c, 0x0FFFFFFF if i == need - 1 else nr[i + 1])
            for c in old_chain:
                fat_set(c, 0)
            new_start = nr[0]
        e = bytearray(r(ot_off, 32))
        e[20:22] = struct.pack('<H', (new_start >> 16) & 0xFFFF)
        e[26:28] = struct.pack('<H', new_start & 0xFFFF)
        e[28:32] = struct.pack('<I', len(new_data))
        w(ot_off, bytes(e))
        print('updated: start cl=%d size=%d' % (new_start, len(new_data)))

    rb = read_chain(new_start)[:len(new_data)]
    print('verify: readback %d bytes, match=%s' % (len(rb), rb == new_data))


if __name__ == '__main__':
    main()
