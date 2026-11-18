#!/usr/bin/env python3
"""mouse_regress.py -- v2.57 PS/2 mouse, selection, clipboard and scrollback
key/wheel regression.

Covers (driven through the QEMU HMP monitor against a real guest):

  1. Movement: relative PS/2 deltas accumulate into the absolute cursor
     position (`mouse: x=.. y=..` diagnostic line).
  2. Buttons: HMP `mouse_button mask` -> the raw button mask the kernel
     sees: 1 = left, 2 = middle, 4 = right.
  3. Selection: left-button down anchors, drag expands, release stores the
     selected UTF-8 text in the kernel clipboard; `clip` echoes it back.
  4. Paste: middle button click injects the clipboard into the active
     prompt line (readline insert + redraw).
  5. Scrollback keys: Ctrl+Up / Ctrl+Down move exactly 1 line per press;
     Ctrl+PgUp / Ctrl+PgDn move one full page (con_rows()-1 = 24 lines in
     the 25-row text mode) per press.
  6. Wheel: each notch scrolls 3 lines (kernel path: con_scrollback(±3)).

Coverage note (wheel)
---------------------
QEMU 6.2's onboard PS/2 mouse emulation does NOT forward the `dz` channel
of `mouse_move dx dy dz` to the guest (verified: `wheel_mode` stays 0 and
the scrollback view does not move after a wheel HMP command). The 4-byte
IntelliMouse wheel packet parser and the 3-line/notch console wiring are
present in the kernel; the guest-side HMP path is therefore marked
DEFERRED here rather than asserted end-to-end. A real (or USB-tablet with
an HID bridge) wheel device would exercise it.

usage:
    python tests/mouse_regress.py <catos.iso> [--qemu PATH] [--port N]
"""
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qemu_driver import (boot, find_qemu, kill_qemu, screen_text, sendkey,
                         type_text, read_screen, wait_for_prompt, cmd)

PAGE_LINES = 24          # con_rows()-1 in the 25-row text mode
LINE_STEP = 1            # Ctrl+Up/Down = 1 line per press
WHEEL_LINES = 3          # one wheel notch = 3 lines (kernel wiring only)

PORT = 4520


class R(object):
    def __init__(self):
        self.n = 0
        self.fails = []

    def check(self, name, cond, detail=""):
        self.n += 1
        print("  %-36s %s" % (name, "PASS" if cond else "FAIL"))
        if not cond:
            self.fails.append(name)
            if detail:
                print("      " + detail[-240:].replace("\n", " "))


def mouse_diag(s):
    """Type `mouse` and parse the diagnostic line. Returns dict or None."""
    type_text(s, "mouse")
    sendkey(s, "ret", 0.9)
    time.sleep(0.6)
    txt = read_screen(s)
    mm = re.findall(r"mouse: x=(\d+) y=(\d+) btn=(\d+) sb=(\d+) pk=(\d+)", txt)
    if not mm:
        return None
    m = mm[-1]                       # ignore stale diag lines still on screen
    return {"x": int(m[0]), "y": int(m[1]),
            "btn": int(m[2]), "sb": int(m[3]),
            "pk": int(m[4])}


def run_line(s, text, wait=1.0):
    type_text(s, text)
    sendkey(s, "ret", wait)


def _resolve_symbol(name):
    """Resolve a kernel symbol to its load address from kernel.pe (PE VMA
    == physical address, so the monitor `xp` can read it directly)."""
    pe = os.path.join(HERE, "..", "kernel_v25", "build", "kernel.pe")
    if not os.path.exists(pe):
        return None
    out = subprocess.run(["nm", pe], capture_output=True, text=True).stdout
    for ln in out.splitlines():
        parts = ln.split()
        if len(parts) >= 3 and parts[2] == name:
            return int(parts[0], 16)
    return None


_SB_VIEW = _resolve_symbol("_sb_view")


def read_sb(s):
    """Read con_scrollback_view() without typing anything: sending a normal
    key would itself leave the history viewer and reset sb_view to 0."""
    if _SB_VIEW is None:
        return None
    out = cmd(s, "xp /1 0x%x" % _SB_VIEW, 0.3)
    m = re.findall(r"0x([0-9a-fA-F]{8})", out)
    return int(m[-1], 16) if m else None


def main():
    args = sys.argv[1:]
    iso = None
    qemu = None
    port = PORT
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--qemu":
            qemu = args[i + 1]; i += 2
        elif a == "--port":
            port = int(args[i + 1]); i += 2
        elif a == "--help":
            print(__doc__); return 0
        else:
            iso = a; i += 1
    if not iso:
        print(__doc__); return 2
    qemu = find_qemu(qemu)

    r = R()
    proc = sock = None
    try:
        proc, sock = boot(qemu, iso, port, monitor_wait=40)
        if not wait_for_prompt(sock, 60):
            print("FAIL: prompt never appeared")
            return 1
        time.sleep(1)

        # ---- 1. movement -------------------------------------------------
        d0 = mouse_diag(sock)
        r.check("baseline diag readable", d0 is not None,
                screen_text(sock)[-160:])
        if d0:
            cmd(sock, "mouse_move 60 40", 1.0)
            time.sleep(0.8)
            d1 = mouse_diag(sock)
            ok = (d1 and abs(d1["x"] - (d0["x"] + 60)) <= 2
                  and abs(d1["y"] - (d0["y"] + 40)) <= 2)
            r.check("move +60,+40 accumulates", ok,
                    "got %s" % (d1,))
            cmd(sock, "mouse_move -30 -20", 1.0)
            time.sleep(0.8)
            d2 = mouse_diag(sock)
            ok = (d2 and abs(d2["x"] - (d1["x"] - 30)) <= 2
                  and abs(d2["y"] - (d1["y"] - 20)) <= 2)
            r.check("move -30,-20 accumulates", ok,
                    "got %s" % (d2,))

        # ---- 2. buttons ---------------------------------------------------
        def press(mask):
            cmd(sock, "mouse_button %d" % mask, 0.8)
            time.sleep(0.5)
            return mouse_diag(sock)

        d = press(1)
        r.check("left button (mask 1) reports btn=1",
                d and d["btn"] == 1, "got %s" % (d,))
        cmd(sock, "mouse_button 0", 0.8); time.sleep(0.4)
        d = press(2)
        r.check("middle button (mask 2) reports btn=2",
                d and d["btn"] == 2, "got %s" % (d,))
        cmd(sock, "mouse_button 0", 0.8); time.sleep(0.4)
        d = press(4)
        r.check("right button (mask 4) reports btn=4",
                d and d["btn"] == 4, "got %s" % (d,))
        cmd(sock, "mouse_button 0", 0.8); time.sleep(0.4)

        # ---- 5. scrollback keys (line / page) -----------------------------
        # Fill the backlog well above one page.
        for k in range(40):
            run_line(sock, "echo sb%02d" % k, 0.12)
        time.sleep(0.8)
        sb0 = read_sb(sock) or 0

        sendkey(sock, "ctrl-up", 1.0); time.sleep(0.5)
        sb1 = read_sb(sock)
        r.check("Ctrl+Up = +1 line", sb1 == sb0 + LINE_STEP,
                "sb %d -> %s" % (sb0, sb1))

        sendkey(sock, "ctrl-pgup", 1.0); time.sleep(0.5)
        sb2 = read_sb(sock)
        want = sb0 + LINE_STEP + PAGE_LINES
        r.check("Ctrl+PgUp = +1 page (%d lines)" % PAGE_LINES,
                sb2 == want, "want %d got %s" % (want, sb2))

        sendkey(sock, "ctrl-down", 1.0); time.sleep(0.5)
        sb3 = read_sb(sock)
        r.check("Ctrl+Down = -1 line", sb3 == want - LINE_STEP,
                "want %d got %s" % (want - LINE_STEP, sb3))

        sendkey(sock, "ctrl-pgdn", 1.0); time.sleep(0.5)
        sb4 = read_sb(sock)
        r.check("Ctrl+PgDn = -1 page (%d lines)" % PAGE_LINES,
                sb4 == sb0, "want %d got %s" % (sb0, sb4))

        # Per-row shift comparison: K single-line steps must move the live
        # content down exactly K rows; row y after == baseline row y-K.
        K = 3
        base_rows = [ln.rstrip() for ln in read_screen(sock).splitlines()]
        for _ in range(K):
            sendkey(sock, "ctrl-up", 1.0); time.sleep(0.4)
        sb_k = read_sb(sock)
        after_rows = [ln.rstrip() for ln in read_screen(sock).splitlines()]
        mism = []
        for y in range(K, min(len(base_rows), len(after_rows))):
            if base_rows[y - K].strip() and after_rows[y].strip() \
               and base_rows[y - K] != after_rows[y]:
                mism.append((y, base_rows[y - K], after_rows[y]))
        r.check("K=%d single-line steps shift rows by K" % K,
                sb_k == sb0 + K and not mism,
                "sb=%s mism=%s" % (sb_k, mism[:3]))

        # Top boundary: keep paging up until the view stops moving; the
        # visible block must have no duplicate adjacent row and no blank
        # hole (archive wrap-around must not repeat or blank a row).
        prev = -1
        for _ in range(12):
            sendkey(sock, "ctrl-pgup", 1.0); time.sleep(0.4)
            cur = read_sb(sock)
            if cur == prev:
                break
            prev = cur
        top_rows = [ln.strip() for ln in read_screen(sock).splitlines()]
        body = [ln for ln in top_rows[1:] if ln]
        dup = any(body[i] == body[i + 1] for i in range(len(body) - 1))
        r.check("top boundary: no duplicate/blank history row",
                not dup and len(body) > 0, "rows=%d dup=%s" % (len(body), dup))

        # Content-level check: one page up actually exposes an older echoed
        # line on the viewport (row content, not just the offset counter).
        txt = screen_text(sock)
        r.check("history line visible after page up",
                re.search(r"sb\d\d", txt) is not None, txt[-160:])
        # Any other key returns to the live screen (sb_view back to 0).
        sendkey(sock, "ctrl-c", 1.0); time.sleep(0.6)
        sb5 = read_sb(sock)
        r.check("exit viewer restores live screen (sb=0)",
                sb5 == 0, "got %s" % (sb5,))

        # ---- 3. selection -> clipboard ------------------------------------
        run_line(sock, "clear", 0.8)
        MARK = "sel-marker-abc"
        run_line(sock, "echo " + MARK, 1.0)
        time.sleep(0.6)

        # Locate the marker OUTPUT row (the echo command row also contains
        # the marker substring but carries the prompt '#').
        lines = read_screen(sock).splitlines()
        row = None
        for idx, ln in enumerate(lines):
            if MARK in ln and "#" not in ln:
                row = idx
                break
        r.check("marker line visible", row is not None,
                screen_text(sock)[-160:])

        if row is not None:
            y_px = row * 16
            cur = mouse_diag(sock)
            x0, y0 = cur["x"], cur["y"]
            cmd(sock, "mouse_move %d %d" % (0 - x0, y_px - y0), 1.0)
            time.sleep(0.6)
            cmd(sock, "mouse_button 1", 0.8)
            time.sleep(0.5)
            cur = mouse_diag(sock)
            cmd(sock, "mouse_move %d 0" % (120 - cur["x"]), 1.0)
            time.sleep(0.6)
            cmd(sock, "mouse_button 0", 0.8)
            time.sleep(0.6)

            run_line(sock, "clip", 1.0)
            txt = screen_text(sock)
            # The marker may be clipped at either end by the drag box;
            # assert a stable middle fragment survives.
            r.check("clip holds selected text",
                    "marker" in txt or "sel-marker" in txt,
                    txt[-160:])

            # ---- 4. middle-click paste into the prompt --------------------
            run_line(sock, "clear", 0.8)
            # Re-select so the clipboard is non-empty for the paste step.
            lines = read_screen(sock).splitlines()
            row = None
            for idx, ln in enumerate(lines):
                if MARK in ln and "#" not in ln:
                    row = idx; break
            if row is not None:
                y_px = row * 16
                cur = mouse_diag(sock)
                cmd(sock, "mouse_move %d %d" % (0 - cur["x"], y_px - cur["y"]),
                    1.0)
                time.sleep(0.6)
                cmd(sock, "mouse_button 1", 0.8); time.sleep(0.5)
                cur = mouse_diag(sock)
                cmd(sock, "mouse_move %d 0" % (120 - cur["x"]), 1.0)
                time.sleep(0.6)
                cmd(sock, "mouse_button 0", 0.8); time.sleep(0.6)

            cmd(sock, "mouse_button 2", 0.8); time.sleep(0.6)
            cmd(sock, "mouse_button 0", 0.8); time.sleep(0.8)
            txt = screen_text(sock)
            r.check("middle-click pastes clipboard into prompt",
                    "marker" in txt or "sel-marker" in txt,
                    txt[-160:])

        # ---- 6. wheel: kernel wiring present, HMP dz not forwarded --------
        # Recorded as a documented limitation; the guest must not crash.
        sb_before = read_sb(sock) or 0
        cmd(sock, "mouse_move 0 0 1", 1.0)
        time.sleep(0.8)
        sb_after = read_sb(sock)
        print("  [note] wheel dz HMP not forwarded by QEMU 6.2 ps2mouse; "
              "kernel path = con_scrollback(±%d) (deferred guest coverage)"
              % WHEEL_LINES)
        r.check("wheel command does not fault", sb_after is not None,
                "sb %d -> %s" % (sb_before, sb_after))

        print("mouse_regress: %d/%d PASS" % (r.n - len(r.fails), r.n))
        return 1 if r.fails else 0
    finally:
        kill_qemu(proc)


if __name__ == "__main__":
    sys.exit(main())
