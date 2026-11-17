#!/bin/bash
# Full regression sweep after the night-merge (v2.42). Sequential: several
# drivers share FAT volume images, so guests must not overlap.
set -u
cd "$(dirname "$0")/.."
export PATH=/home/user/bin:$PATH
Q=/home/user/bin/qemu-system-i386
ISO=kernel_v25/catos.iso
LOGDIR=tests/regress_logs
mkdir -p "$LOGDIR"

run() {
    local name="$1"; shift
    echo "===== $name ====="
    python3 "$@" > "$LOGDIR/$name.log" 2>&1
    local rc=$?
    echo "$name exit=$rc"
    return $rc
}

run qemu    tests/qemu_regress.py    "$ISO" --qemu "$Q" --port 4707
run guess   tests/shell_guess_regress.py "$ISO" --qemu "$Q" --port 4450
run fsrec   tests/fsrec_regress.py   "$ISO" --qemu "$Q" --port 4711
run lfn     tests/lfn_regress.py     "$ISO" --qemu "$Q" --port 4712
run recur   tests/recur_regress.py   "$ISO" --qemu "$Q" --port 4713
run jobctl  tests/jobctl_regress.py   "$ISO" --qemu "$Q" --port 4468
run caps    tests/caps_regress.py    "$ISO" --qemu "$Q"
run user    tests/user_regress.py    "$ISO" --qemu "$Q"
run disk    tests/disk_regress.py    "$ISO" --qemu "$Q" --port 4714
run env     tests/env_regress.py     "$ISO" --qemu "$Q"
run heap    tests/heap_regress.py    --qemu "$Q" --port 4716
run size    tests/size_regress.py    "$ISO" --qemu "$Q"
run ce      tests/ce_regress.py

echo "===== sweep done ====="
