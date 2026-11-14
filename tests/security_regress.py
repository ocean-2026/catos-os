#!/usr/bin/env python3
"""security_regress.py -- on-metal regression for the v2.16 fix round.

Covers the bugs found in the 2026-11-12 QEMU screenshots:

  #97  ACL path lookup was case-sensitive / not normalised, so FAT's
       case-insensitive names let `run /BIN/X.CE` bypass `chmod 000`.
  #98  cmd_chmod passed uninitialised fgid/fmode into acl_set().
  #99  The ISO under test had been re-packed WITHOUT recompiling, so
       #96's PERM_EXEC check was never in the kernel that was "tested".
       Also: a denied spawn returned -2 (ENOENT) instead of -13 (EACCES),
       which made "permission denied" indistinguishable from "no such
       file" -- the source of the false-positive screenshot assertions.
  #100 `vdiag` clobbered screen row 3 with its glyphA dump and rewrote
       the VGA palette (black background turned purple).
  #101 CJK punctuation had no glyphs in hz_font and degraded to '?'.

As with every suite here: own monitor port, own disk image, and the
guest is killed by PID (never `taskkill /IM`, which kills other guests).
"""
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qemu_driver import (boot, cmd, find_qemu, kill_qemu,  # noqa: E402
                         screen_text, sendkey, type_text,
                         wait_for_prompt)

REPO = os.path.dirname(HERE)
MONITOR_PORT = 4476                 # own port
DISK = os.path.join(HERE, "qemu_disk_security.img")
ISO = os.path.join(REPO, "kernel_v25", "catos.iso")
PPM = os.path.join(HERE, "qemu_shots", "security.ppm")


class Report(object):
    def __init__(self):
        self.n = 0
        self.fails = []

    def check(self, name, cond, detail=""):
        self.n += 1
        ok = bool(cond)
        print("  %-32s %s" % (name, "PASS" if ok else "FAIL"))
        if not ok:
            self.fails.append(name)
            if detail:
                print("      " + detail[-240:].replace("\n", " "))


def win_path(p):
    """QEMU is a native Windows program and cannot open MSYS /tmp paths."""
    return os.path.abspath(p).replace("\\", "/")


def drain(s):
    """Drop stale HMP echo before a command whose output we parse."""
    s.setblocking(False)
    try:
        while True:
            if not s.recv(65536):
                break
    except BlockingIOError:
        pass
    finally:
        s.setblocking(True)


def hmp(s, c, wait=1.0):
    drain(s)
    return cmd(s, c, wait)


def run(s, c, wait=1.3):
    type_text(s, c)
    sendkey(s, "ret")
    time.sleep(wait)
    return screen_text(s)


def clean_run(s, c, wait=1.3):
    """Clear first: screen_text() returns the WHOLE scrollback, so a
    previous `exit(0)` would otherwise make a denial look like success.
    This was the exact false-positive pattern in the screenshots."""
    type_text(s, "clear")
    sendkey(s, "ret")
    time.sleep(0.8)
    return run(s, c, wait)


def vram_chars(s):
    """Raw character codes from 0xB8000 (see qemu_driver.read_screen)."""
    out = hmp(s, "xp /4000 0xb8000", 2.0)
    data = bytearray()
    for line in out.splitlines():
        line = line.strip()
        if not re.match(r"^[0-9a-fA-F]{8}:", line):
            continue
        for word in re.findall(r"0x([0-9a-fA-F]{8})", line):
            v = int(word, 16)
            data += bytes((v & 0xFF, (v >> 8) & 0xFF,
                           (v >> 16) & 0xFF, (v >> 24) & 0xFF))
    return [data[i] for i in range(0, len(data), 2)]


def bg_pixel(s):
    """Screendump is async and lags a frame: do it twice, take the last."""
    for _ in range(2):
        hmp(s, "screendump " + win_path(PPM), 0.8)
        time.sleep(1.6)
    d = open(PPM, "rb").read()
    m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+(\d+)\s", d)
    if not m:
        return None
    w, h, _ = [int(v) for v in m.groups()]
    raw = d[m.end():m.end() + w * h * 3]
    i = (380 * w + 700) * 3
    return tuple(raw[i:i + 3])


def main():
    iso = os.path.abspath(ISO)
    qemu = find_qemu(sys.argv[sys.argv.index("--qemu") + 1]
                     if "--qemu" in sys.argv else None)
    # Fresh volume every run: ACL entries from a previous run would make
    # the chmod cases non-idempotent.
    if os.path.exists(DISK):
        os.remove(DISK)
    with open(DISK, "wb") as f:
        f.truncate(64 * 1024 * 1024)
    os.makedirs(os.path.dirname(PPM), exist_ok=True)

    proc, sock = boot(qemu, iso, MONITOR_PORT, disk=win_path(DISK))
    r = Report()
    try:
        wait_for_prompt(sock)
        # -- fresh FAT32 volume ----------------------------------------
        # cmd_format uses kb_readline: 'y' needs a following Enter, and
        # the kernel reinstalls all built-in apps afterwards (typing
        # during that window is swallowed).
        type_text(sock, "format")
        sendkey(sock, "ret")
        time.sleep(1.2)
        type_text(sock, "y")
        sendkey(sock, "ret")
        t = ""
        for _ in range(30):
            time.sleep(1.0)
            t = screen_text(sock)
            if "Installed missing built-in" in t or "Aborted" in t:
                break
        r.check("format-ok", "Installed missing built-in" in t, t)

        # -- #99 build-freshness: the check must be IN this kernel ------
        # A positive control alone is not enough; a permanently-denying
        # kernel would pass every negative case.  Require 755 to execute.
        t = run(sock, "cp /bin/captest.ce /bin/xtest.ce")
        r.check("cp-ok", "err" not in t[-200:], t)
        run(sock, "chmod 755 /bin/xtest.ce")
        t = clean_run(sock, "run /bin/xtest.ce", 2.0)
        r.check("positive-control-755-exec", "exit(0)" in t, t)

        # -- #97 all path variants of chmod 000 must be denied ----------
        run(sock, "chmod 000 /bin/xtest.ce")
        variants = [
            ("deny-exact", "run /bin/xtest.ce"),
            ("deny-case-variant", "run /BIN/XTEST.CE"),
            ("deny-dotdot-variant", "run /bin/../bin/xtest.ce"),
            ("deny-double-slash", "run /bin//xtest.ce"),
        ]
        for name, c in variants:
            t = clean_run(sock, c, 1.8)
            r.check(name, "err -13" in t and "exit(0)" not in t, t)
        run(sock, "cd /bin")
        t = clean_run(sock, "run XTEST.CE", 1.8)
        r.check("deny-relative-path", "err -13" in t and "exit(0)" not in t, t)
        run(sock, "cd /")

        # -- #98 no stack garbage in the persisted ACL -------------------
        t = clean_run(sock, "cat /etc/acl")
        m = re.search(r"bin/xtest\.ce:(\d+):(\d+):(\d+)", t)
        r.check("acl-entry-no-garbage",
                m is not None and m.group(2) == "0" and m.group(3) == "0", t)

        # restoring 755 must work again (no sticky denial)
        run(sock, "chmod 755 /bin/xtest.ce")
        t = clean_run(sock, "run /bin/xtest.ce", 2.0)
        r.check("restore-755-exec", "exit(0)" in t, t)

        # -- #101 CJK punctuation ---------------------------------------
        t = clean_run(sock, "punctest", 2.0)
        r.check("glyph-count-6961", "glyphs=6961" in t, t)
        chars = vram_chars(sock)
        nq = sum(1 for c in chars if c == 0x3F)
        nwide = sum(1 for c in chars if 0x80 <= c <= 0xFF)
        r.check("no-ascii-question-marks", nq == 0,
                "VRAM 0x3F count=%d" % nq)
        r.check("wide-glyph-slots-used", nwide >= 20,
                "VRAM wide cells=%d" % nwide)

        # -- #100 vdiag must not clobber the screen or palette -----------
        type_text(sock, "clear")
        sendkey(sock, "ret")
        time.sleep(0.8)
        run(sock, "ls /bin", 2.0)
        bg_before = bg_pixel(sock)
        run(sock, "vdiag", 2.5)
        bg_after = bg_pixel(sock)
        t = screen_text(sock)
        r.check("vdiag-background-black",
                bg_before == bg_after and bg_after is not None
                and max(bg_after) < 40,
                "before=%r after=%r" % (bg_before, bg_after))
        r.check("vdiag-keeps-ls-output", "[FILE]" in t and "entries" in t, t)
        r.check("vdiag-glyphA-inline", "glyphA" in t, t)

        # -- nothing crashed --------------------------------------------
        t = clean_run(sock, "faultinfo")
        r.check("no-fatal-fault",
                "no fatal fault" in t.lower() or "no fault" in t.lower(), t)
    finally:
        kill_qemu(proc)

    print("\nsecurity_regress: %d/%d PASS%s" %
          (r.n - len(r.fails), r.n,
           "" if not r.fails else "  FAILED: " + ", ".join(r.fails)))
    return 0 if not r.fails else 1


if __name__ == "__main__":
    sys.exit(main())
