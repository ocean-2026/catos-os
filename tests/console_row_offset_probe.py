# -*- coding: utf-8 -*-
"""console_row_offset_probe.py -- controlled repro for the "wrong newline /
vertical row shift" defect seen on the CatOS terminal.

Method
------
1. Serve crafted HTML bodies over plain HTTP on the host (slirp, guest ->
   10.0.2.2:8099).  The cases mirror the shape of the baidu pages that
   exposed the bug (script/noscript lines) plus edge-length lines
   (79/80/81/99/100/101 chars), CRLF line endings, tabs, blank lines, long
   paragraphs, and enough lines to force many console scrolls.
2. Boot the guest in the graphics console (gfx24 -- the default backend;
   with -vga vmware it takes the VMware SVGA path) and fetch each case with
   `curl http://10.0.2.2:8099/cN.html`.
3. screendump (PPM), decode every 8x16 cell back to a character by matching
   the committed IBM VGA 8x16 font (kernel_v25/src/font_ascii.inc).  Take
   two shots 1.5 s apart: if they differ, the screen never settled (host/
   LFB lag), which is its own finding.
4. Compare the decoded rows against the *simulated* console layout: output
   indent (2 spaces on a line start), wrap at the backend column count, in
   this order, exactly as render.c is supposed to do it.

A clean run means "grid and pixels are consistent" for that case.  A diff
tells us, character by character, whether a character was lost, wrapped one
column early/late, or landed on the wrong row.

usage: python3 console_row_offset_probe.py [iso] [--vga vmware|std]
"""
import http.server
import os
import socket
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from qemu_driver import find_qemu, kill_qemu, sendkey, type_text  # noqa: E402
from qemu_driver import _stage_image  # noqa: E402

MONITOR_PORT = 4517
HTTP_PORT = 8099
CELL_W, CELL_H = 8, 16


# --------------------------------------------------------------------------
# test corpus
# --------------------------------------------------------------------------
def case_basic():
    """The page that exposed the bug, byte-for-byte shape."""
    return (
        "<html>\n"
        "<head>\n"
        "<script>  location.replace(location.href.replace(\"https://\",\"http://\"))  </script>\n"
        "<noscript><meta http-equiv=\"refresh\" content=\"0;url=http://www.baidu.com/\"></noscript>\n"
        "</head>\n"
        "<body>\n"
        "<a href=\"http://www.baidu.com/\">Moved Permanently</a>.\n"
        "</body>\n"
        "</html>\n"
    ).encode()


def case_crlf():
    return case_basic().replace(b"\n", b"\r\n")


def case_widths():
    lines = []
    for n in (79, 80, 81, 99, 100, 101, 120, 160, 200):
        head = "L%03d|" % n
        lines.append(head + "=" * (n - len(head)))
    return ("\n".join(lines) + "\n").encode()


def case_para():
    """One very long paragraph: wraps repeatedly, no newlines at all."""
    s = "A" * 40 + "abcdefghij" * 30
    return (s + "\n").encode()


def case_scroll():
    """60 short lines -- several full-screen scrolls, the scroll path under
    load; last line is a unique marker."""
    lines = ["S%02d|0123456789abcdefghijklmnopqrstuv" % i for i in range(60)]
    lines.append("MARKER-SCROLL-END")
    return ("\n".join(lines) + "\n").encode()


def case_tabs():
    return ("T1|\ta\tb\tc\n"
            "T2|0123456789012345678901234567890123456789\tTAIL\n").encode()


def case_blank():
    return ("<html>\n\n\n\n<body>\n  \n<div>\nMARKER-BLANK\n</div>\n</body>\n</html>\n").encode()


def case_mixed():
    """Long line right after a scroll boundary, then short lines."""
    return (("X" * 100 + "\n") * 40 + "MARKER-MIXED\n" + "<hr>\n").encode()


CASES = [
    ("c0", case_basic),
    ("c1", case_crlf),
    ("c2", case_widths),
    ("c3", case_para),
    ("c4", case_scroll),
    ("c5", case_tabs),
    ("c6", case_blank),
    ("c7", case_mixed),
]


class Handler(http.server.BaseHTTPRequestHandler):
    pages = {}

    def do_GET(self):
        key = self.path.strip("/").split(".")[0]
        body = self.pages.get(key)
        if body is None:
            self.send_response(404)
            self.end_headers()
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *a):
        pass


def start_server():
    Handler.pages = {name: fn() for name, fn in CASES}
    httpd = http.server.ThreadingHTTPServer(("127.0.0.1", HTTP_PORT), Handler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    return httpd


# --------------------------------------------------------------------------
# PPM + font OCR
# --------------------------------------------------------------------------
class PPM:
    def __init__(self, path):
        with open(path, "rb") as f:
            data = f.read()
        parts = data.split(b"\n", 3)
        assert parts[0].strip() == b"P6", "not a binary PPM"
        self.w, self.h = [int(x) for x in parts[1].split()]
        self.px = parts[3]

    def get(self, x, y):
        o = (y * self.w + x) * 3
        return self.px[o], self.px[o + 1], self.px[o + 2]


def load_font():
    path = os.path.join(REPO, "kernel_v25", "src", "font_ascii.inc")
    font = {}
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line.startswith("{0x"):
                continue
            rows = [int(v, 16) for v in
                    line[line.index("{") + 1:line.index("}")].split(",")]
            if len(rows) != 16:
                continue
            ch = chr(int(line.split("/*")[1].split()[0], 16))
            font[ch] = rows
    assert len(font) >= 95, "font table parse failed: %d glyphs" % len(font)
    return font


def glyph_masks(font):
    out = {}
    for ch, rows_b in font.items():
        if ch == " ":
            continue
        m = 0
        for j in range(15):
            m |= (rows_b[j] & 0xFE) << (16 * j)
        out[ch] = m
    return out


def read_grid(ppm, masks, cols, rows):
    out = []
    for gy in range(rows):
        line = []
        for gx in range(cols):
            ox, oy = gx * CELL_W, gy * CELL_H
            if ox + CELL_W > ppm.w or oy + CELL_H > ppm.h:
                line.append(" ")
                continue
            counts = {}
            for j in range(CELL_H):
                for i in range(CELL_W):
                    c = ppm.get(ox + i, oy + j)
                    counts[c] = counts.get(c, 0) + 1
            bg = max(counts, key=counts.get)
            mask = 0
            nink = 0
            for j in range(15):
                row = 0
                for i in range(7):
                    if ppm.get(ox + i, oy + j) != bg:
                        row |= 1 << (7 - i)
                        nink += 1
                mask |= row << (16 * j)
            if nink == 0:
                line.append(" ")
                continue
            best, bestd = "?", 10 ** 9
            for ch, m in masks.items():
                d = bin(m ^ mask).count("1")
                if d < bestd:
                    best, bestd = ch, d
            line.append(best if bestd <= 10 else "?")
        out.append("".join(line))
    return out


# --------------------------------------------------------------------------
# console simulator (what render.c is supposed to do)
# --------------------------------------------------------------------------
def simulate(stream, cols, rows, indent=True):
    grid = [[" "] * cols for _ in range(rows)]
    cx = cy = 0
    line_start = True

    def put(ch):
        nonlocal cx, cy
        if cx + 1 > cols:
            cx = 0
            cy += 1
        while cy >= rows:
            grid.pop(0)
            grid.append([" "] * cols)
            cy = rows - 1
        grid[cy][cx] = ch
        cx += 1

    for ch in stream:
        if ch == "\r":
            cx = 0
            line_start = True
            continue
        if ch == "\n":
            cx = 0
            cy += 1
            while cy >= rows:
                grid.pop(0)
                grid.append([" "] * cols)
                cy = rows - 1
            line_start = True
            continue
        if ch == "\t":
            nx = (cx + 4) & ~3
            while cx < nx:
                put(" ")
            line_start = False
            continue
        if line_start and indent:
            put(" ")
            put(" ")
        line_start = False
        put(ch)
    return ["".join(r).rstrip() for r in grid]


def trim_blank(rows):
    out = list(rows)
    while out and not out[0].strip():
        out.pop(0)
    while out and not out[-1].strip():
        out.pop()
    return out


# --------------------------------------------------------------------------
# monitor helpers
# --------------------------------------------------------------------------
def shot(sock, path, wait=1.2):
    if os.path.exists(path):
        os.remove(path)
    sock.sendall(("screendump %s\n" % path).encode())
    time.sleep(wait)
    for _ in range(10):
        if os.path.exists(path) and os.path.getsize(path) > 1024:
            time.sleep(0.2)
            return PPM(path)
        time.sleep(0.2)
    raise RuntimeError("screendump produced nothing: %s" % path)


def send_line(sock, text, wait=1.5):
    type_text(sock, text)
    sendkey(sock, "ret", wait)


def compare_case(name, body, ppm1, ppm2, masks, cols, rows):
    """Return (settled, diffs, nrows) for one case.

    The body is located on screen by matching its first simulated row,
    falling back to tail alignment on its last row when the head has scrolled
    off.  Rows outside the body (command echo, header, footer, prompt) are
    ignored.  Leading indentation is significant, so rows are only right-
    stripped.
    """
    g1 = read_grid(ppm1, masks, cols, rows)
    g2 = read_grid(ppm2, masks, cols, rows)
    settled = g1 == g2
    g = [r.rstrip() for r in g1]
    exp = [r.rstrip() for r in trim_blank(simulate(body.decode(errors="replace"),
                                                   cols, rows))]
    start = None
    # Tail-first: the last body row is the unique one in our corpus (repetitive
    # probes like the 40 identical X lines would happy-match at the head).
    for i in range(len(g) - 1, -1, -1):
        if g[i] == exp[-1]:
            start = i - (len(exp) - 1)
            break
    if start is None:
        for i in range(len(g)):
            if g[i] == exp[0]:
                start = i
                break
    if start is None:
        return settled, [(-1, exp[0], "<body not found>")], len(exp)
    diffs = []
    for i in range(len(exp)):
        j = start + i
        if j < 0:
            continue            # scrolled off the top: not on screen any more
        got = g[j] if j < len(g) else "<off screen>"
        if got != exp[i]:
            diffs.append((i, exp[i], got))
    return settled, diffs, max(0, min(len(exp), len(g) - start))


def main():
    args = [a for a in sys.argv[1:]]
    vga = "vmware"
    if "--vga" in args:
        i = args.index("--vga")
        vga = args[i + 1]
        del args[i:i + 2]
    iso = os.path.abspath(args[0] if args
                          else os.path.join(REPO, "kernel_v25", "catos.iso"))
    qemu = find_qemu(None)
    if not qemu:
        print("FATAL: qemu-system-i386 not found")
        return 2
    masks = glyph_masks(load_font())
    httpd = start_server()
    print("host HTTP 127.0.0.1:%d, %d cases, vga=%s"
          % (HTTP_PORT, len(CASES), vga))

    outdir = os.path.join(REPO, "artifacts", "con-wrap")
    os.makedirs(outdir, exist_ok=True)

    iso_i = _stage_image(iso)
    cmdline = [
        qemu, "-m", "256", "-no-reboot", "-display", "none",
        "-vga", vga,
        "-netdev", "user,id=n0", "-device", "pcnet,netdev=n0",
        "-monitor", "tcp:127.0.0.1:%d,server,nowait" % MONITOR_PORT,
        "-cdrom", iso_i, "-boot", "order=d",
    ]
    proc = subprocess.Popen(cmdline, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    sock = None
    rc = 0
    try:
        for _ in range(40):
            if proc.poll() is not None:
                print("FATAL: qemu exited rc=%r" % proc.returncode)
                return 3
            try:
                sock = socket.create_connection(("127.0.0.1", MONITOR_PORT),
                                                timeout=5)
                break
            except OSError:
                time.sleep(2)
        if sock is None:
            print("FATAL: no monitor")
            return 3
        sock.settimeout(20.0)
        print("booting gfx24 (%s) ..." % vga)
        time.sleep(24)
        ppm = shot(sock, os.path.join(outdir, "boot.ppm"))
        cols, rows = ppm.w // CELL_W, ppm.h // CELL_H
        print("frame %dx%d -> grid %dx%d" % (ppm.w, ppm.h, cols, rows))

        send_line(sock, "netcfg dhcp", 3.0)
        time.sleep(3.0)

        for name, fn in CASES:
            body = fn()
            send_line(sock, "clear", 0.8)
            time.sleep(0.4)
            send_line(sock, "curl http://10.0.2.2:%d/%s.html" % (HTTP_PORT, name), 2.5)
            time.sleep(1.6)
            p1 = shot(sock, os.path.join(outdir, "%s-1.ppm" % name), 1.0)
            time.sleep(1.6)
            p2 = shot(sock, os.path.join(outdir, "%s-2.ppm" % name), 1.0)
            settled, diffs, n = compare_case(name, body, p1, p2, masks, cols, rows)
            print("-- %s: %d rows compared, settled=%s, diffs=%d"
                  % (name, n, settled, len(diffs)))
            for i, e, g in diffs[:8]:
                print("   row %d\n     exp|%s\n     got|%s" % (i, e, g))
            if diffs or not settled:
                rc = 1
    finally:
        try:
            if sock:
                sock.close()
        except OSError:
            pass
        kill_qemu(proc)
        httpd.shutdown()
    print("== exit %d ==" % rc)
    return rc


if __name__ == "__main__":
    sys.exit(main())
