#!/bin/sh
# build_ce.sh <app.c> - build a CE app from tests/ with the canonical
# extapp toolchain.  Output: kernel_v25/extapp/bin/<name>.ce
#
# This used to be a full copy of extapp/build_ce.sh with every path relative
# to tests/ -- but tests/ has no ce.ld, no ce_stub.c and its mkce.py path
# pointed at a directory that does not exist.  It could never build anything
# (exit 127).  Keep ONE real toolchain; this file is only a wrapper.
set -e

SRC=$1
if [ -z "$SRC" ]; then echo "usage: sh tests/build_ce.sh app.c"; exit 1; fi
ROOT=$(cd "$(dirname "$0")/.." && pwd)
CEBASE=$(cd "$(dirname "$0")" && pwd)
# A bare filename means a source in tests/, no matter where this is called from.
case "$SRC" in
    */*) ABS=$(cd "$(dirname "$SRC")" && pwd)/$(basename "$SRC") ;;
    *)   ABS="$CEBASE/$SRC" ;;
esac
if [ ! -f "$ABS" ]; then echo "ERROR: no such source: $SRC (looked at $ABS)"; exit 1; fi
exec sh "$ROOT/kernel_v25/extapp/build_ce.sh" "$ABS"
