#!/usr/bin/env python3
"""Inject a file into /BIN/ on the Catos_Better_better disk (RSVD=33 layout).
Usage: inject_any.py <img> <src-file> <8.3-NAME>   e.g. TEST_AB.CE
Layout assumed: RSVD=33, SPC=8, NFATS=2, FAT_SZ=8192 (a 4 GiB sparse raw disk
formatted by the CatOS kernel)."""
import struct, sys

IMG = sys.argv[1]
SRC = sys.argv[2]
NAME83 = sys.argv[3]
# build a standard 8.3 entry (8-char name + 3-char ext, NO dot, spaces padded)
if '.' in NAME83:
    base, ext = NAME83.split('.', 1)
else:
    base, ext = NAME83, ''
name_bytes = (base[:8].ljust(8) + ext[:3].ljust(3)).encode()
BPS = 512
SPC = 8
RSVD = 33
NFATS = 2
FAT_SZ = 8192
DATA_OFF = (RSVD + NFATS * FAT_SZ) * BPS
FAT_OFF = RSVD * BPS
TOTAL_CL = 4 * 1024 * 1024 * 1024 // BPS // SPC

def r(off, n):
    with open(IMG, 'rb') as f:
        f.seek(off); return f.read(n)

def w(off, data):
    with open(IMG, 'r+b') as f:
        f.seek(off); f.write(data)

def fat_get(cl):
    return struct.unpack('<I', r(FAT_OFF + cl * 4, 4))[0] & 0x0FFFFFFF

def fat_set(cl, val):
    val &= 0x0FFFFFFF
    ent = struct.unpack('<I', r(FAT_OFF + cl * 4, 4))[0] & 0xF0000000
    w(FAT_OFF + cl * 4, struct.pack('<I', ent | val))
    w(FAT_OFF + FAT_SZ * BPS + cl * 4, struct.pack('<I', ent | val))

def cluster_off(cl):
    return DATA_OFF + (cl - 2) * SPC * BPS

def chain(cl):
    out = []
    seen = set()
    while 2 <= cl < 0x0FFFFFF8 and cl not in seen:
        seen.add(cl); out.append(cl)
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
    new_data = open(SRC, 'rb').read()
    print('%s: %d bytes' % (NAME83, len(new_data)))
    need = (len(new_data) + SPC * BPS - 1) // (SPC * BPS)
    bin_entries = find_dir_entries(2, b'BIN        ')
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
