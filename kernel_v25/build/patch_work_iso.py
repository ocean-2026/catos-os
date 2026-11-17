import os
import sys
import struct
B = 2048
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'build', 'catos_work.iso')
KERNEL = os.path.join(ROOT, 'build', 'kernel.bin')
OUT = os.path.join(ROOT, 'catos.iso')

# #99 guard: this script only PATCHES the prebuilt kernel.bin into the
# GRUB template -- it never compiles anything.  Running it directly after
# editing src/ silently produces an ISO with OLD code, which is exactly
# how the #96 PERM_EXEC tests "passed" against a kernel that did not
# contain the check at all (build/task.o was an hour older than task.c).
# Refuse to package a stale kernel unless the caller says --force.
def assert_kernel_fresh():
    kmt = os.path.getmtime(KERNEL)
    newer = []
    for sub in ('src', 'include'):
        d = os.path.join(ROOT, sub)
        for fn in os.listdir(d):
            if fn.endswith(('.c', '.h', '.inc')):
                p = os.path.join(d, fn)
                if os.path.getmtime(p) > kmt + 1:
                    newer.append(os.path.relpath(p, ROOT))
    if not newer or '--force' in sys.argv:
        return
    print('ERROR: build/kernel.bin is OLDER than %d source file(s):' % len(newer))
    for p in sorted(newer)[:10]:
        print('   ', p)
    if len(newer) > 10:
        print('    ... and %d more' % (len(newer) - 10))
    print('Run `sh build.sh iso` (full rebuild) first.  Only pass --force')
    print('if you deliberately want to package the stale kernel.')
    raise SystemExit(2)

assert_kernel_fresh()

w = open(SRC, 'rb').read()
kernel = open(KERNEL, 'rb').read()

# v2.46: the embedded GRUB config waited `timeout=5` at the menu before
# booting the default entry -- ~5 s of dead menu countdown on every boot.
# Boot straight into CatOS.  The pattern is asserted unique so a GRUB
# image upgrade that changes the config layout fails loudly here instead
# of silently keeping the old delay.
_old = b"set timeout=5"
_new = b"set timeout=0"
if w.count(_old) == 1:
    w = w.replace(_old, _new)
elif w.count(_new) == 0:
    raise SystemExit("embedded grub.cfg: neither 'timeout=5' nor "
                     "'timeout=0' pattern found; inspect the template")
ksec = (len(kernel) + B - 1) // B
kernel_lba = len(w) // B            # append after current volume
newvol = kernel_lba + ksec
print('kernel %d bytes, %d sectors, will be at LBA %d, new vol %d' % (len(kernel), ksec, kernel_lba, newvol))

# 1. patch PVD volume space
pvd = bytearray(w[B*16:B*17])
struct.pack_into('<I', pvd, 80, newvol)
struct.pack_into('>I', pvd, 84, newvol)

# 2. rebuild root dir @19: parse existing records, append kernel.bin
rd = bytearray(w[B*19:B*20])
def parse_records(data):
    recs = []
    off = 0
    while off < B:
        l = data[off]
        if l == 0:
            break
        recs.append(bytes(data[off:off+l]))
        off += l
    return recs

recs = parse_records(rd)
print('root dir records: %d' % len(recs))
for r in recs:
    nl = r[32]
    print('  len=%d lba=%d size=%d flags=%d name=%r' % (r[0], struct.unpack_from('<I', r, 2)[0], struct.unpack_from('<I', r, 10)[0], r[25], r[33:33+nl]))

def dir_record(lba, size, flags, name):
    nb = name if isinstance(name, bytes) else name.encode('ascii')
    rec = bytearray([33 + len(nb), 0])
    rec += struct.pack('<I', lba) + struct.pack('>I', lba)
    rec += struct.pack('<I', size) + struct.pack('>I', size)
    rec += bytes([2026-1900, 10, 14, 0, 0, 0, 0])
    rec += bytes([flags, 0, 0])
    rec += struct.pack('<H', 1) + struct.pack('>H', 1)
    rec += bytes([len(nb)]) + nb
    if len(rec) % 2 == 1:
        rec += b'\x00'
        rec[0] += 1
    return bytes(rec)

newrec = dir_record(kernel_lba, len(kernel), 0, b'kernel.bin;1')
recs.append(newrec)
new_rd = bytearray()
for r in recs:
    new_rd += r
if len(new_rd) > B:
    raise SystemExit('root dir overflow')
new_rd += bytes(B - len(new_rd))

# 3. assemble
out = bytearray(w)
out[B*16:B*17] = pvd
out[B*19:B*20] = new_rd
out += kernel
out += bytes((ksec * B) - len(kernel))
assert len(out) == newvol * B, 'len %d vs %d' % (len(out), newvol*B)
open(OUT, 'wb').write(bytes(out))
print('OK -> %s (%d bytes)' % (OUT, len(out)))
