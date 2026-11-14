#!/usr/bin/env python3
"""test_mkce.py -- regression suite for the CE01 image wrapper (mkce.py).

Both copies of mkce.py (kernel_v25/build and kernel_v25/extapp/build) must:

  * read CE_MAX_IMAGE / CE_MAX_BSS from include/ce.h, not a stale local copy
    (the *source* of the limits is asserted, not just their values -- the
    built-in fallback happens to have the same numbers, so a value-only
    assertion would pass even when ce.h is unreachable);
  * accept images above the old 64 KiB cap (the v2.5 kernel allows 256 KiB --
    a self-hosted cc.ce / CC2.CE is >64 KiB, so the old cap blocked bootstrap);
  * reject at build time everything task_spawn_ce() would reject at load time
    (image, bss, api_offset), instead of failing with err -5 inside QEMU;
  * reject malformed / out-of-range arguments with a clean `error:` line,
    never a Python traceback, and never leave a half-written .ce behind;
  * emit a correct 20-byte CE01 header and preserve the raw image verbatim.

usage:
    python tests/test_mkce.py
"""
import importlib.util
import os
import re
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CE_H = os.path.join(ROOT, "kernel_v25", "include", "ce.h")
MKCE_FILES = [
    os.path.join(ROOT, "kernel_v25", "build", "mkce.py"),
    os.path.join(ROOT, "kernel_v25", "extapp", "build", "mkce.py"),
]

passed = failed = 0


def check(name, cond, detail=""):
    global passed, failed
    if cond:
        print("PASS %-34s %s" % (name, detail))
        passed += 1
    else:
        print("FAIL %-34s %s" % (name, detail))
        failed += 1


def c_int(s):
    s = s.strip()
    return int(s, 16) if s.lower().startswith("0x") else int(s, 10)


def ce_h_limits():
    with open(CE_H, "r", encoding="utf-8", errors="replace") as f:
        text = f.read()
    vals = []
    for key in ("CE_MAX_IMAGE", "CE_MAX_BSS"):
        m = re.search(r"^\s*#define\s+%s\s+(0[xX][0-9a-fA-F]+|[0-9]+)[uUlL]*"
                      % key, text, re.MULTILINE)
        vals.append(c_int(m.group(1)) if m else None)
    return tuple(vals)


def load_module(path):
    spec = importlib.util.spec_from_file_location("mkce_under_test", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def run_mkce(mkce, raw, out, bss, api):
    """Run with hex-formatted arguments (what build_ce.sh actually passes)."""
    return run_mkce_raw(mkce, raw, out, "%x" % bss, "%x" % api)


def run_mkce_raw(mkce, raw, out, bss_str, api_str):
    cmd = [sys.executable, mkce, raw, out, bss_str, api_str]
    return subprocess.run(cmd, capture_output=True, text=True)


def expect_reject(tag, name, mkce, raw, out, bss_arg, api_arg, keyword,
                  raw_args=False):
    """Remove stale output, run, assert a clean rejection with no output file."""
    if os.path.exists(out):
        os.remove(out)
    if raw_args:
        r = run_mkce_raw(mkce, raw, out, bss_arg, api_arg)
    else:
        r = run_mkce(mkce, raw, out, bss_arg, api_arg)
    ok = (r.returncode != 0
          and keyword in r.stdout
          and "Traceback" not in r.stderr
          and not os.path.exists(out))
    check(tag + ": " + name, ok,
          "rc=%d out=%r err=%r" % (r.returncode, r.stdout.strip()[:70],
                                   r.stderr.strip()[:40]))


def make_raw(path, size):
    with open(path, "wb") as f:
        f.write(bytes((i * 7 + 3) & 0xff for i in range(size)))
    return path


def header_of(path):
    with open(path, "rb") as f:
        d = f.read(20)
    if len(d) != 20 or d[:4] != b"CE01":
        return None
    return struct.unpack_from("<IIII", d, 4)


def main():
    limits = ce_h_limits()
    print("== ce.h limits: image=0x%x bss=0x%x ==" % limits)
    check("ce.h limits present", None not in limits, str(limits))
    if None in limits:
        return 1
    max_image, max_bss = limits

    # the two wrappers must stay byte-identical (they are used by different
    # build scripts; a divergence reintroduces the 64 KiB cap in one path)
    with open(MKCE_FILES[0], "rb") as a, open(MKCE_FILES[1], "rb") as b:
        check("two mkce.py identical", a.read() == b.read())

    for mkce in MKCE_FILES:
        tag = "build" if os.sep + "extapp" + os.sep not in mkce else "extapp"
        mod = load_module(mkce)
        got = mod.load_limits()
        source_ok = (got[2] != "built-in fallback"
                     and os.path.normcase(os.path.abspath(got[2]))
                     == os.path.normcase(os.path.abspath(CE_H)))
        check(tag + ": limits really from ce.h",
              got[:2] == limits and source_ok,
              "vals=%s source=%s" % (got[:2], got[2]))

        with tempfile.TemporaryDirectory(prefix="test_mkce_") as td:
            raw = os.path.join(td, "in.bin")
            out = os.path.join(td, "out.ce")

            # --- images above the legacy 64 KiB cap must now build ---------
            for size, label in ((1024, "small"), (0x10000, "legacy 64K cap"),
                                (81920, "80K (CC2.CE class)"),
                                (max_image, "exact max")):
                make_raw(raw, size)
                r = run_mkce(mkce, raw, out, 0, 0x10)
                h = header_of(out) if r.returncode == 0 else None
                check(tag + ": " + label,
                      r.returncode == 0 and h == (0, 0, 0x10, size),
                      "rc=%d hdr=%s" % (r.returncode, h))
                if h:
                    with open(out, "rb") as f, open(raw, "rb") as g:
                        blob, src = f.read(), g.read()
                    check(tag + ": payload " + label,
                          blob == b"CE01" + struct.pack("<IIII", 0, 0, 0x10, size) + src)

            make_raw(raw, max_image + 1)
            expect_reject(tag, "reject image > max", mkce, raw, out,
                          0, 0x10, "image too big")

            # --- bss must be checked at build time, mirroring the loader ---
            make_raw(raw, 1024)
            r = run_mkce(mkce, raw, out, max_bss, 0x10)
            check(tag + ": accept bss == max",
                  r.returncode == 0 and header_of(out) == (0, max_bss, 0x10, 1024))
            expect_reject(tag, "reject bss > max", mkce, raw, out,
                          max_bss + 1, 0x10, "bss too big")

            # --- api_offset + 4 must fit inside the image (loader: err -5) ---
            make_raw(raw, 1024)
            r = run_mkce(mkce, raw, out, 0, 1020)
            check(tag + ": api slot at last 4B",
                  r.returncode == 0 and header_of(out) == (0, 0, 1020, 1024))
            expect_reject(tag, "reject api slot past end", mkce, raw, out,
                          0, 1021, "api_offset")
            # uint32 wrap: the kernel computes api+4 in uint32_t and would let
            # 0xFFFFFFFC slip through; the build tool must be stricter.
            expect_reject(tag, "reject uint32-wrap api", mkce, raw, out,
                          0, 0xFFFFFFFC, "api_offset")

            # --- argument parsing: clean errors, no tracebacks ------------
            make_raw(raw, 1024)
            r = run_mkce_raw(mkce, raw, out, "0X0", "0x10")
            check(tag + ": 0x prefix / upper hex",
                  r.returncode == 0 and header_of(out) == (0, 0, 0x10, 1024))
            for label, bss_s, api_s, kw in (
                    ("reject non-hex bss", "zz", "10", "not a hex number"),
                    ("reject empty bss", "", "10", "not a hex number"),
                    ("reject non-hex api", "0", "zz", "not a hex number"),
                    ("reject negative bss", "-1", "10", "out of uint32"),
                    ("reject negative api", "0", "-1", "out of uint32"),
                    ("reject bss > uint32", "100000000", "10", "out of uint32"),
                    ("reject api > uint32", "0", "100000000", "out of uint32")):
                expect_reject(tag, label, mkce, raw, out, bss_s, api_s, kw,
                              raw_args=True)

    print("\n%d passed, %d failed" % (passed, failed))
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
