#!/usr/bin/env python3
"""mkpkg.py - pack a CatOS CE application into a standard .catpkg package.

P4 package-manager host-side tool.  A .catpkg is a small self-describing
container: a fixed metadata header followed by the raw CE01 image.  The
CatOS guest-side `pkg install` command reads the header, validates the
magic and copies the payload to /bin/<cmd>.ce.

Layout (little-endian, all fields NUL-padded ASCII):

    offset  0  magic[8]      "CPKG01\\0"
    offset  8  name[16]      package name (index key, e.g. "bc")
    offset 24  version[16]  e.g. "1.0"
    offset 40  cmd[16]       installed command name -> /bin/<cmd>.ce
    offset 56  desc[80]     one-line description
    offset 136 payload_len  uint32 LE, bytes following this header
    offset 140 payload       raw .ce image (CE01 header included)

usage:
  python3 mkpkg.py IN.ce OUT.catpkg --name bc --version 1.0 \\
          --cmd bc --desc "tiny integer calculator"

The same header constants are mirrored in kernel_v25/src/shell.c
(PKG_HDR_*); keep the two in sync.
"""
import argparse
import os
import struct
import sys

MAGIC = b"CPKG01\x00\x00"
HDR_NAME = 16
HDR_VERSION = 16
HDR_CMD = 16
HDR_DESC = 80
HEADER_SIZE = 8 + HDR_NAME + HDR_VERSION + HDR_CMD + HDR_DESC + 4


def pad_field(text, width):
    b = text.encode("ascii", "replace")[:width - 1]
    return b + b"\x00" * (width - len(b))


def build_header(name, version, cmd, desc, payload_len):
    return (MAGIC
            + pad_field(name, HDR_NAME)
            + pad_field(version, HDR_VERSION)
            + pad_field(cmd, HDR_CMD)
            + pad_field(desc, HDR_DESC)
            + struct.pack("<I", payload_len))


def main():
    ap = argparse.ArgumentParser(description="pack a CatOS .catpkg")
    ap.add_argument("ce", help="input .ce image")
    ap.add_argument("catpkg", help="output .catpkg")
    ap.add_argument("--name", required=True, help="package name")
    ap.add_argument("--version", default="1.0", help="package version")
    ap.add_argument("--cmd", required=True,
                    help="installed command (/bin/<cmd>.ce)")
    ap.add_argument("--desc", default="", help="one-line description")
    args = ap.parse_args()

    if not os.path.exists(args.ce):
        print("ERROR: missing %s" % args.ce, file=sys.stderr)
        return 2

    payload = open(args.ce, "rb").read()
    if payload[:4] != b"CE01":
        print("WARNING: %s does not start with the CE01 magic" % args.ce,
              file=sys.stderr)

    header = build_header(args.name, args.version, args.cmd,
                          args.desc, len(payload))
    assert len(header) == HEADER_SIZE

    with open(args.catpkg, "wb") as fh:
        fh.write(header)
        fh.write(payload)
    print("OK -> %s (%d bytes payload, %d bytes total)"
          % (args.catpkg, len(payload), HEADER_SIZE + len(payload)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
