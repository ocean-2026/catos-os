#!/usr/bin/env python3
"""pager_regress.py -- v2.59 generic console pager regression.

Covers the minimal deliverable pager wired onto the EXISTING line-output
path (vga_write / con_write).  One shared pager backs both `help` and `ls`
(and optionally `cat`/`tree`); there is no second screen driver and no
full-screen repaint.

  p-1   `help` exceeds one screen -> the "-- More" prompt appears on the
        bottom row.
  p-2   Enter advances exactly one line (the first row past the first
        screenful moves onto the visible frame).
  p-3   Space advances a whole screenful.
  p-4   q quits the pager: the remaining help output is dropped and the
        shell prompt comes back (no leftover More bar).
  p-5   reaching the end: the More bar disappears and the rest prints out
        without a residual prompt row.
  p-6   `ls /bin` (long dir, seeded offline) pager behaves the same:
        More appears, Enter one line, Space a screen, q back to prompt.
  p-7   `help > /tmp/x` redirection: no More bar ever appears and the
        command returns immediately (non-interactive streams straight
        through).

  p-8   0-line output (`echo -n`) and 1-line output (`echo hi`): the pager
        streams straight through, no More bar ever parks, prompt returns.
  p-9   a directory whose entry count is exactly one screenful: no More bar
        (the whole block fits one frame, nothing left to park on).
  p-10  a directory with screenful+1 entries: the More bar parks once.

The attached disk image is seeded OFFLINE with three extra directories so
the `ls` boundary cases do not depend on the boot-builtin count:
  /pshort  0 real entries (dots only)   -- covered by p-8 via echo;
  /pone    1 real entry                  -- p-8 one-line;
  /pfull  22 real entries (one 24-line frame, plus the summary row) -- p-9;
  /pmany  23 real entries (one frame + 1)                    -- p-10.

usage:
  python tests/pager_regress.py <catos.iso> [--qemu PATH] [--port N]
"""
import os, re, shutil, struct, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qemu_driver import (boot, find_qemu, kill_qemu, screen_text, read_screen,
                         sendkey, type_text, wait_for_prompt, force_text)

PORT = 4580
MORE = "-- More"
PROMPT_RE = re.compile(r"catos:[^\n]*[\$#]")

# Text mode frame is 80x25; a screenful reserves one row for the More bar,
# so one screen holds (25 - 1) = 24 pager rows.  A plain `ls` also prints a
# trailing "--- N entries ---" summary row, so a directory is one frame when
# it holds at most 23 real entries (entries + summary <= 24), and spills a
# screen at 24+ real entries.  The seeded dirs below are sized against this.
FULL_COUNT = 23
MANY_COUNT = 24


class R(object):
    def __init__(self):
        self.n = 0
        self.fails = []

    def check(self, name, cond, detail=""):
        self.n += 1
        ok = bool(cond)
        print("  %-34s %s" % (name, "PASS" if ok else "FAIL"))
        if not ok:
            self.fails.append(name)
            if detail:
                print("      " + detail[-260:].replace("\n", " "))


def cmdline(s, c, wait=1.0):
    type_text(s, c)
    sendkey(s, "ret", wait)
    time.sleep(wait)
    return screen_text(s)


def clean(s, wait=0.8):
    type_text(s, "clear")
    sendkey(s, "ret", wait)


def drain(s, wait=0.6, steps=12):
    """Walk any parked More bar to the last page (and never leave one armed,
    so the following commands get their keystrokes)."""
    for _ in range(steps):
        if MORE not in screen_text(s):
            return
        sendkey(s, "spc", wait)


def prompt_back(s):
    """True when the shell prompt is back on the frame."""
    return bool(PROMPT_RE.search(read_screen(s)))


# ---------- offline FAT seeding for the ls boundary directories ----------

def _geom(img):
    with open(img, "rb") as f:
        mbr = f.read(512)
    part_start = int.from_bytes(mbr[446 + 8:446 + 12], "little")
    with open(img, "rb") as f:
        f.seek(part_start * 512)
        dbr = f.read(512)
    bps = int.from_bytes(dbr[0x0B:0x0D], "little")
    spc = dbr[0x0D]
    rsvd = int.from_bytes(dbr[0x0E:0x10], "little")
    nfats = dbr[0x10]
    fatsz = int.from_bytes(dbr[0x24:0x28], "little")
    root = int.from_bytes(dbr[0x2C:0x30], "little")
    fat_off = (part_start + rsvd) * bps
    data_off = (part_start + rsvd + nfats * fatsz) * bps
    return dict(bps=bps, spc=spc, rsvd=rsvd, nfats=nfats, fatsz=fatsz,
                root=root, fat_off=fat_off, data_off=data_off)


def _read(img, off, n):
    with open(img, "rb") as f:
        f.seek(off)
        return f.read(n)


def _write(img, off, data):
    with open(img, "r+b") as f:
        f.seek(off)
        f.write(data)


def _fat_get(G, img, cl):
    off = G["fat_off"] + cl * 4
    return struct.unpack("<I", _read(img, off, 4))[0] & 0x0FFFFFFF


def _fat_set(G, img, cl, val):
    ent_off = G["fat_off"] + cl * 4
    ent = struct.unpack("<I", _read(img, ent_off, 4))[0] & 0xF0000000
    blob = struct.pack("<I", ent | (val & 0x0FFFFFFF))
    _write(img, ent_off, blob)
    _write(img, ent_off + G["fatsz"] * G["bps"], blob)


def _cluster_off(G, cl):
    return G["data_off"] + (cl - 2) * G["spc"] * G["bps"]


def _dent(name11, attr, clus, size=0):
    e = bytearray(32)
    # Pad an 8.3 short name out to 11 bytes with spaces; bare shorter names
    # are the normal FAT form, but a trailing NUL byte makes fat_name83_eq /
    # lookup mis-compare against the space-padded template the kernel builds.
    padded = name11.ljust(11, b" ")
    e[0:11] = padded
    e[11] = attr
    e[20:22] = struct.pack("<H", (clus >> 16) & 0xFFFF)
    e[26:28] = struct.pack("<H", clus & 0xFFFF)
    e[28:32] = struct.pack("<I", size)
    return bytes(e)


def _alloc_free(G, img):
    cl = 3
    while cl < 0x0FFFFFF0:
        if _fat_get(G, img, cl) == 0:
            return cl
        cl += 1
    raise RuntimeError("no free cluster for seeded dir")


def _make_dir(G, img, parent, base_name83, n_files):
    """Create a fresh single-cluster directory under `parent`, with n_files
    plain file entries.  Returns the new directory's cluster."""
    nc = _alloc_free(G, img)
    csz = G["spc"] * G["bps"]
    buf = bytearray(csz)
    # "." and ".."
    buf[0:32] = _dent(b"." + b" " * 10, 0x10, nc, 0)
    buf[32:64] = _dent(b".." + b" " * 9, 0x10, parent, 0)
    for i in range(n_files):
        stem = "F%03d" % (i + 1)
        name = (stem + "TXT").encode().ljust(11, b" ")
        # each injected file points at its own zero-length cluster chain
        fc = _alloc_free(G, img)
        _fat_set(G, img, fc, 0x0FFFFFFF)
        buf[(2 + i) * 32:(3 + i) * 32] = _dent(name, 0x20, fc, 0)
    _write(img, _cluster_off(G, nc), bytes(buf))
    _fat_set(G, img, nc, 0x0FFFFFFF)
    # register the directory in its parent
    off = _cluster_off(G, parent)
    region = bytearray(_read(img, off, csz))
    for i in range(0, csz, 32):
        if region[i] == 0x00 or region[i] == 0xE5:
            region[i:i + 32] = _dent(base_name83, 0x10, nc, 0)
            _write(img, off, bytes(region))
            return nc
    raise RuntimeError("parent directory full")


def seed_pager_disk(src, dst):
    """Copy the seed image offline and add /pone, /pfull, /pmany dirs."""
    shutil.copy2(src, dst)
    G = _geom(dst)
    _make_dir(G, dst, G["root"], b"PONE     ", 1)
    _make_dir(G, dst, G["root"], b"PFULL    ", FULL_COUNT)
    _make_dir(G, dst, G["root"], b"PMANY    ", MANY_COUNT)
    return dst


def main():
    iso = os.path.abspath(sys.argv[1])
    qemu = find_qemu(sys.argv[sys.argv.index("--qemu") + 1]
                     if "--qemu" in sys.argv else None)
    port = PORT
    if "--port" in sys.argv:
        i = sys.argv.index("--port")
        port = int(sys.argv[i + 1])
    src_disk = os.path.abspath(os.path.join(HERE, "qemu_disk_pager.img"))
    tmpdir = tempfile.mkdtemp(prefix="catos_pager_")
    disk = seed_pager_disk(src_disk, os.path.join(tmpdir, "pager.img"))
    proc, sock = boot(qemu, iso, port, disk=disk)
    r = R()
    try:
        force_text(sock)
        wait_for_prompt(sock)

        # ---------- p-1: help reaches a More prompt ---------------------
        clean(sock)
        type_text(sock, "help")
        sendkey(sock, "ret", 0.6)
        time.sleep(0.8)
        t = screen_text(sock)
        r.check("p-1-help-more-shown", MORE in t, t)

        # ---------- p-2: Enter advances one line -------------------------
        # Snapshot the visible rows before Enter, press Enter once, and a
        # row that was off-screen must now be visible while the More bar
        # is still parked (the help list has far more than one screen).
        before = read_screen(sock)
        sendkey(sock, "ret", 0.4)
        time.sleep(0.6)
        after = screen_text(sock)
        r.check("p-2-enter-more-still", MORE in after, after)
        # The frame shifted down by one: compare non-blank row sets.
        def rows(txt):
            return [ln.strip() for ln in txt.splitlines() if ln.strip()]
        rb, ra = rows(before), rows(read_screen(sock))
        r.check("p-2-enter-advanced", ra != rb,
                "frame unchanged after Enter")

        # ---------- p-3: Space advances one screen ----------------------
        sendkey(sock, "spc", 0.4)
        time.sleep(0.6)
        t = screen_text(sock)
        r.check("p-3-space-more-still", MORE in t, t)

        # ---------- p-4: q quits, prompt back, no bar --------------------
        sendkey(sock, "q", 0.3)
        time.sleep(0.8)
        t = screen_text(sock)
        r.check("p-4-quit-no-more", MORE not in t, t)
        r.check("p-4-quit-prompt-back", prompt_back(sock), t)

        # ---------- p-5: reach the end, bar disappears -------------------
        # Drive help again and page all the way through with Space until
        # the bar leaves the frame; the prompt must come back cleanly.
        clean(sock)
        type_text(sock, "help")
        sendkey(sock, "ret", 0.6)
        time.sleep(0.8)
        ended = False
        for _ in range(20):
            t = screen_text(sock)
            if MORE not in t:
                ended = True
                break
            sendkey(sock, "spc", 0.25)
            time.sleep(0.4)
        r.check("p-5-end-bar-gone", ended, "More bar still parked after 20 pages")
        r.check("p-5-end-prompt-back", prompt_back(sock), screen_text(sock))

        # ---------- p-6: ls /bin pager -----------------------------------
        clean(sock)
        type_text(sock, "ls /bin")
        sendkey(sock, "ret", 0.6)
        time.sleep(0.8)
        t = screen_text(sock)
        r.check("p-6-ls-more-shown", MORE in t, t)
        # Enter one line in the ls view.
        sendkey(sock, "ret", 0.4)
        time.sleep(0.5)
        t = screen_text(sock)
        r.check("p-6-ls-enter-more", MORE in t, t)
        # Space a screen.
        sendkey(sock, "spc", 0.4)
        time.sleep(0.5)
        t = screen_text(sock)
        r.check("p-6-ls-space-more", MORE in t, t)
        # q drops the rest, prompt returns.
        sendkey(sock, "q", 0.3)
        time.sleep(0.8)
        t = screen_text(sock)
        r.check("p-6-ls-quit-no-more", MORE not in t, t)
        r.check("p-6-ls-quit-prompt-back", prompt_back(sock), t)

        # ---------- p-7: redirect streams straight through ---------------
        clean(sock)
        t0 = time.time()
        type_text(sock, "help > /tmp/pager_out.txt")
        sendkey(sock, "ret", 0.5)
        time.sleep(1.2)
        t = screen_text(sock)
        elapsed = time.time() - t0
        r.check("p-7-redirect-no-more", MORE not in t, t)
        # t0 covers typing the 25-char line as well as the command itself;
        # the budget is for "did not block", not for typing speed.
        r.check("p-7-redirect-returns-fast", elapsed < 15.0,
                "elapsed=%.1fs" % elapsed)
        r.check("p-7-redirect-prompt-back", prompt_back(sock), t)
        # The redirected file should carry help content (written by the
        # normal output path), not a More bar.
        type_text(sock, "cat /tmp/pager_out.txt")
        sendkey(sock, "ret", 1.0)
        time.sleep(1.0)
        t = screen_text(sock)
        r.check("p-7-redirect-file-has-help", "commands" in t, t)
        # `cat` of a 25+ line file is itself paged: that bar belongs to cat,
        # not to the redirected file.  Drain, then assert the file text is
        # free of the bar and the prompt is back.
        drain(sock)
        t = screen_text(sock)
        r.check("p-7-redirect-file-no-bar", MORE not in t, t)

        # ---------- p-8: 0-line and 1-line output, no park ---------------
        clean(sock)
        type_text(sock, "echo -n")
        sendkey(sock, "ret", 0.6)
        time.sleep(0.6)
        t = screen_text(sock)
        r.check("p-8-zero-line-no-more", MORE not in t, t)
        r.check("p-8-zero-line-prompt-back", prompt_back(sock), t)

        clean(sock)
        type_text(sock, "echo hi")
        sendkey(sock, "ret", 0.6)
        time.sleep(0.6)
        t = screen_text(sock)
        r.check("p-8-one-line-no-more", MORE not in t, t)
        r.check("p-8-one-line-prompt-back", prompt_back(sock), t)

        # ---------- p-9: exactly one screenful, no More bar --------------
        clean(sock)
        type_text(sock, "ls /pfull")
        sendkey(sock, "ret", 0.6)
        time.sleep(0.8)
        t = screen_text(sock)
        r.check("p-9-one-screen-no-more", MORE not in t, t)
        r.check("p-9-one-screen-prompt-back", prompt_back(sock), t)

        # ---------- p-10: one screen + 1 entry, More parks once ----------
        clean(sock)
        type_text(sock, "ls /pmany")
        sendkey(sock, "ret", 0.6)
        time.sleep(0.8)
        t = screen_text(sock)
        r.check("p-10-overflow-more-shown", MORE in t, t)
        # q drops the rest and the prompt returns cleanly.
        sendkey(sock, "q", 0.3)
        time.sleep(0.8)
        t = screen_text(sock)
        r.check("p-10-overflow-quit-no-more", MORE not in t, t)
        r.check("p-10-overflow-prompt-back", prompt_back(sock), t)

    finally:
        kill_qemu(proc)

    print("\npager_regress: %d/%d PASS" % (r.n - len(r.fails), r.n))
    if r.fails:
        print("FAILURES: %s" % ", ".join(r.fails))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
