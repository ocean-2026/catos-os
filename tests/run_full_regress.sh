#!/bin/bash
# Full regression sweep (v2.42 -> v2.59). Sequential by design: several
# drivers share FAT volume images and several suites read the 0xB8000 text
# plane, so guests must not overlap.
#
# Ports are FIXED per suite (see docs/regression-ports.md).  Never let a
# suite pick a dynamic port: with several clones/agents alive on the same
# host a "borrowed" port turns into a false failure (HMP empty reply,
# "screendump produced no file", "prompt never appeared").
#
# A flock guard keeps two sweeps (e.g. two clones) from running at once.
set -u
cd "$(dirname "$0")/.."
export PATH=/home/user/bin:$PATH
Q=/home/user/bin/qemu-system-i386
ISO=kernel_v25/catos.iso
LOGDIR=tests/regress_logs
mkdir -p "$LOGDIR"

exec 9> "$LOGDIR/.regress.lock"
if ! flock -w 60 9; then
    echo "another regression sweep holds $LOGDIR/.regress.lock - aborting"
    exit 3
fi

FAILED=()

run() {
    local name="$1"; shift
    echo "===== $name ====="
    python3 "$@" > "$LOGDIR/$name.log" 2>&1
    local rc=$?
    echo "$name exit=$rc"
    if [ $rc -ne 0 ]; then FAILED+=("$name"); fi
    return 0
}

# --- core suites (152/152 baseline) ---------------------------------------
run qemu     tests/qemu_regress.py         "$ISO" --qemu "$Q" --port 4707
run guess    tests/shell_guess_regress.py  "$ISO" --qemu "$Q" --port 4450
run fsrec    tests/fsrec_regress.py        "$ISO" --qemu "$Q" --port 4711
run lfn      tests/lfn_regress.py          "$ISO" --qemu "$Q" --port 4712
run recur    tests/recur_regress.py        "$ISO" --qemu "$Q" --port 4713
run jobctl   tests/jobctl_regress.py       "$ISO" --qemu "$Q" --port 4468
run group    tests/group_regress.py        "$ISO" --qemu "$Q" --port 4469
run daemon   tests/daemon_regress.py       "$ISO" --qemu "$Q" --port 4467
run caps     tests/caps_regress.py         "$ISO" --qemu "$Q"
run user     tests/user_regress.py         "$ISO" --qemu "$Q"
run disk     tests/disk_regress.py         "$ISO" --qemu "$Q" --port 4714
run env      tests/env_regress.py          "$ISO" --qemu "$Q"
run heap     tests/heap_regress.py         --qemu "$Q" --port 4716
run size     tests/size_regress.py         "$ISO" --qemu "$Q"
run ce       tests/ce_regress.py

# --- suites added for v2.56 .. v2.59 --------------------------------------
run videomode tests/videomode_regress.py   "$ISO" --qemu "$Q" --port 4718
run badname    tests/fs_badname_regress.py "$ISO" --qemu "$Q" --port 4719
run mouse      tests/mouse_regress.py      "$ISO" --qemu "$Q" --port 4720
run panic      tests/panic_regress.py      "$ISO" --qemu "$Q" --port 4546
if [ -f tests/pager_regress.py ]; then
    run pager  tests/pager_regress.py      "$ISO" --qemu "$Q" --port 4721
fi
if [ -f tests/ime_regress.py ]; then
    run ime    tests/ime_regress.py        "$ISO" --qemu "$Q" --port 4722
fi
if [ -f tests/gui_pixel_regress.py ]; then
    run guipix tests/gui_pixel_regress.py  "$ISO" --port 4723
fi
if [ -f tests/gui_console_clean.py ]; then
    run guiclean tests/gui_console_clean.py "$ISO" --port 4724
fi

echo "===== sweep done ====="
if [ ${#FAILED[@]} -gt 0 ]; then
    echo "FAILED: ${FAILED[*]}"
    exit 1
fi
echo "ALL SUITES GREEN"
