#!/bin/sh
# Root-level build entry point (compat shim).
#
# The single source of truth is kernel_v25/.  The root-level gfx.c/gfx.h/
# vga.c/shell.c files left over from the 2026-11-05 graphics drop were
# FROZEN SNAPSHOTS, not build inputs -- their contents have been
# integrated into kernel_v25/src/render*.c, be_*.c and shell.c, and the
# snapshot files themselves were removed from the repo on 2026-11-09.
#
# Run from anywhere:  sh build.sh [kernel|iso]
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE/kernel_v25"
exec sh build.sh "$@"
