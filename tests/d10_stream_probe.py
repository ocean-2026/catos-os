#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""d10_stream_probe.py -- D10 (P2) large-package streaming acceptance.

Boots CatOS with an e1000 NIC behind slirp user-net and verifies that
packages far beyond the old 16 KiB static package buffer can be downloaded,
signature-verified, written to disk and executed:

  A. Content-Length route: a ~50 KiB signed .catpkg via `pkg fetch <name>`.
  B. Chunked route:       a same-size package served with
                          `Transfer-Encoding: chunked` (NO Content-Length),
                          fetched by explicit URL -- proving the incremental
                          chunked decoder works instead of falling back to
                          Content-Length framing.

Evidence policy (D10): pass/fail is decided from ON-SCREEN text only.  A
host-side GET log proves the request left the guest; it does not prove the
bytes landed on disk, verified or ran.  Every key step is captured with an
HMP `screendump` (PPM) and converted to PNG in the artifacts dir.

usage:
    python3 tests/d10_stream_probe.py [catos.iso] [artifacts_dir]
"""
import http.server
import os
import re
import shutil
import socket
import subprocess
import sys
import threading
import time
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qemu_driver import (find_qemu, kill_qemu, screen_text, sendkey,  # noqa: E402
                         type_text, wait_for_prompt, _stage_image)  # noqa: E402

REPO = os.path.dirname(HERE)
MONITOR_PORT = int(os.environ.get("D10_MONITOR", "4502"))
HTTP_PORT = int(os.environ.get("D10_HTTP", "8126"))

# The acceptance .ce carries a 48 KiB read-only blob (see extapp/d10big.c),
# so its payload is comfortably above the old 16 KiB PKG_BUF_MAX ceiling and
# well below CE_MAX_IMAGE (256 KiB).  The probe asserts the fetched byte
# count against the old 16 KiB cap rather than a fixed size.
MIN_BIG_BYTES = 16384

BIG_FILE = "big.catpkg"
CHUNK_FILE = "chunk.catpkg"
BIG_NAME = "bigpkg"
CHUNK_NAME = "chunkpkg"

SERVE = {}          # path -> body bytes
ART = os.path.join(REPO, "artifacts", "d10-stream")


# ---------------------------------------------------------------------------
# host-side package preparation
# ---------------------------------------------------------------------------
def ce_image():
    """The well-formed acceptance .ce (48 KiB blob baked into the image)."""
    ce_in = os.path.join(REPO, "kernel_v25", "extapp", "bin", "d10big.ce")
    raw = open(ce_in, "rb").read()
    if raw[:4] != b"CE01":
        raise RuntimeError("d10big.ce missing CE01 magic (build extapp first)")
    if len(raw) <= MIN_BIG_BYTES:
        raise RuntimeError("d10big.ce too small to test the old 16 KiB cap")
    return raw


def build_packages():
    tools = os.path.join(REPO, "kernel_v25", "tools", "mkpkg.py")
    pkg_dir = os.path.join(ART, "repo")
    os.makedirs(pkg_dir, exist_ok=True)
    tmpce = os.path.join(tempfile.gettempdir(), "d10_pad.ce")

    def sign(name, cmd, out_name):
        open(tmpce, "wb").write(ce_image())
        subprocess.run(
            [sys.executable, tools, tmpce, os.path.join(pkg_dir, out_name),
             "--name", name, "--version", "1.0", "--cmd", cmd,
             "--desc", "D10 streamed big package"],
            check=True)

    sign(BIG_NAME, BIG_NAME, BIG_FILE)
    sign(CHUNK_NAME, CHUNK_NAME, CHUNK_FILE)

    lines = ["# D10 stream test repo", "name|version|desc|file",
             "%s|1.0|big Content-Length package|%s" % (BIG_NAME, BIG_FILE),
             "%s|1.0|big chunked package|%s" % (CHUNK_NAME, CHUNK_FILE)]
    SERVE["/repo/PKGS.TXT"] = ("\n".join(lines) + "\n").encode()
    SERVE["/repo/" + BIG_FILE] = open(
        os.path.join(pkg_dir, BIG_FILE), "rb").read()
    SERVE["/repo/" + CHUNK_FILE] = open(
        os.path.join(pkg_dir, CHUNK_FILE), "rb").read()


# ---------------------------------------------------------------------------
# host HTTP server: normal Content-Length route + a chunked route
# ---------------------------------------------------------------------------
class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/repo/" + CHUNK_FILE:
            body = SERVE[self.path]
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            i = 0
            while i < len(body):
                part = body[i:i + 4096]
                self.wfile.write(b"%x\r\n%s\r\n" % (len(part), part))
                i += 4096
            self.wfile.write(b"0\r\n\r\n")
            return
        body = SERVE.get(self.path)
        if body is None:
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *a):
        pass


# ---------------------------------------------------------------------------
# guest interaction helpers
# ---------------------------------------------------------------------------
def wait_for_text(s, needles, timeout=25.0):
    deadline = time.time() + timeout
    last = ""
    while time.time() < deadline:
        last = screen_text(s)
        if all(n in last for n in needles):
            return True, last
        time.sleep(0.5)
    return False, last


def screenshot(s, name):
    """HMP screendump -> PPM, then convert to PNG with PIL."""
    ppm = os.path.join(ART, name + ".ppm")
    png = os.path.join(ART, name + ".png")
    try:
        s.sendall(("screendump %s\n" % ppm).encode())
        time.sleep(1.0)
        from PIL import Image
        Image.open(ppm).save(png)
        print("  evidence: %s" % png)
    except Exception as e:
        print("  screenshot failed for %s: %s" % (name, e))
    return png


def main():
    global ART
    iso = os.path.abspath(sys.argv[1] if len(sys.argv) > 1
                          else os.path.join(REPO, "kernel_v25", "catos.iso"))
    ART = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else ART)
    os.makedirs(ART, exist_ok=True)

    qemu = find_qemu(None)
    if not qemu:
        print("qemu-system-i386 not found")
        return 2

    build_packages()
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", HTTP_PORT), Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    print("host http repo on 127.0.0.1:%d (%d objects)"
          % (HTTP_PORT, len(SERVE)))

    disk = os.path.join(HERE, "qemu_disk_d10.img")
    if not os.path.exists(disk):
        # A freshly truncated 64 MiB disk has no FAT boot/root and the shell
        # never comes up; seed it from the built-in reference image (the
        # guest runs `format` below anyway).
        seed = os.path.join(REPO, "kernel_v25", "catos_disk.img")
        if os.path.exists(seed):
            shutil.copyfile(seed, disk)
        else:
            with open(disk, "wb") as fh:
                fh.truncate(64 * 1024 * 1024)
    iso_i = _stage_image(iso)
    disk_i = _stage_image(disk)
    cmdline = [qemu, "-m", "256", "-no-reboot",
        "-drive", "file=%s,format=raw,if=ide,index=0" % disk_i,
        "-drive", "file=%s,media=cdrom,if=ide,index=1" % iso_i,
        "-boot", "order=d", "-display", "none",
        "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
        "-monitor", "tcp:127.0.0.1:%d,server,nowait" % MONITOR_PORT]
    if os.environ.get("D10_SERIAL"):
        cmdline += ["-serial", "file:" + os.environ["D10_SERIAL"]]
    proc = subprocess.Popen(cmdline, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    s = None
    for _ in range(40):
        if proc.poll() is not None:
            print("FATAL: qemu exited rc=%r" % proc.returncode)
            return 3
        try:
            s = socket.create_connection(("127.0.0.1", MONITOR_PORT), timeout=5)
            break
        except OSError:
            time.sleep(2)
    if s is None:
        print("FATAL: no monitor")
        return 3

    results = []

    def step(name, ok, note=""):
        results.append((name, bool(ok)))
        print("%-4s %-44s %s" % ("PASS" if ok else "FAIL", name, note))

    def do(line, wait=1.5):
        type_text(s, line)
        sendkey(s, "ret", wait)

    try:
        time.sleep(20)
        type_text(s, "text", 0.05)
        sendkey(s, "ret", 0.6)
        sendkey(s, "y", 0.8)
        time.sleep(1.2)
        if not wait_for_prompt(s, timeout=120.0):
            print("FATAL: shell prompt never appeared")
            return 3
        do("format", 1.0)
        sendkey(s, "y", 0.4)
        sendkey(s, "ret", 15.0)
        if not wait_for_prompt(s, timeout=90.0):
            print("FATAL: shell never returned after format")
            return 3

        do("netcfg dhcp", 3.0)
        ok, _ = wait_for_text(s, ["dhcp ok"], 20.0)
        step("netcfg dhcp gets a lease", ok)

        do("clear", 0.5)
        do("pkg repo http://10.0.2.2:%d/repo" % HTTP_PORT, 1.5)
        ok, _ = wait_for_text(s, ["remote repo ->"], 10.0)
        step("pkg repo persists base URL", ok)

        do("clear", 0.5)
        do("pkg update", 5.0)
        ok, _ = wait_for_text(s, ["index updated"], 30.0)
        step("pkg update downloads remote index", ok)

        # --- A. Content-Length route: big package -------------------------
        do("clear", 0.5)
        do("pkg fetch " + BIG_NAME, 6.0)
        ok, txt = wait_for_text(s, ["verifying", "installed " + BIG_NAME], 40.0)
        m = re.search(r"got (\d+) bytes", txt)
        big_n = int(m.group(1)) if m else 0
        step("big pkg fetch >16 KiB (Content-Length)",
             ok and big_n > MIN_BIG_BYTES, "bytes=%d" % big_n)
        screenshot(s, "shot1_fetch_big")

        do("clear", 0.5)
        do(BIG_NAME, 3.0)
        ok, txt = wait_for_text(s, ["D10 BIGPKG OK"], 20.0)
        step("installed big pkg runs", ok, txt[-120:] if not ok else "")
        screenshot(s, "shot2_run_big")

        # --- B. chunked route: no Content-Length --------------------------
        do("clear", 0.5)
        do("pkg fetch http://10.0.2.2:%d/repo/%s" % (HTTP_PORT, CHUNK_FILE), 6.0)
        ok, txt = wait_for_text(s, ["verifying", "installed " + CHUNK_NAME], 40.0)
        m = re.search(r"got (\d+) bytes", txt)
        chunk_n = int(m.group(1)) if m else 0
        step("chunked pkg fetch >16 KiB (Transfer-Encoding: chunked)",
             ok and chunk_n > MIN_BIG_BYTES, "bytes=%d" % chunk_n)
        screenshot(s, "shot3_fetch_chunk")

        do("clear", 0.5)
        do(CHUNK_NAME, 3.0)
        ok, txt = wait_for_text(s, ["D10 BIGPKG OK"], 20.0)
        step("chunked-installed pkg runs", ok, txt[-120:] if not ok else "")
        screenshot(s, "shot4_run_chunk")
    finally:
        kill_qemu(proc)
        srv.shutdown()

    npass = sum(1 for _, ok in results if ok)
    print("========== SUMMARY ==========")
    for name, ok in results:
        print("PASS" if ok else "FAIL", name)
    print("%d/%d passed" % (npass, len(results)))
    return 0 if npass == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
