#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""pkg_remote_v270_probe.py -- v2.70 remote package repository acceptance.

Boots CatOS with an e1000 NIC behind slirp user-net and verifies the full
remote-install chain with a HOST-side HTTP repo (slirp maps 10.0.2.2 ->
host loopback):

  1. `pkg repo http://10.0.2.2:PORT/repo` persists the base URL.
  2. `pkg update` downloads <repo>/PKGS.TXT into the local index.
  3. `pkg fetch <name>` downloads a signed .catpkg over HTTP, verifies its
     RSA-512 signature with the SAME kernel verifier used for local installs,
     and drops the payload into /bin/<cmd>.ce.
  4. The installed app runs (bare-name lookup through PATH=/bin).
  5. A tampered package (payload byte flipped) is REJECTED by signature
     verification -- proving the remote path is not信任 unverified bytes.

usage: python tests/pkg_remote_v270_probe.py [catos.iso]
"""
import http.server
import os
import socket
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qemu_driver import (boot as _qemu_boot, find_qemu,  # noqa: E402
                         kill_qemu, screen_text, sendkey, type_text,
                         wait_for_prompt, _stage_image, read_screen)  # noqa: E402

REPO = os.path.dirname(HERE)
MONITOR_PORT = 4496
HTTP_PORT = 8125
PKGSRC = os.path.abspath(os.path.join(REPO, "kernel_v25", "extapp", "pkg"))

# HTTP tree we serve: /repo/PKGS.TXT + /repo/*.catpkg (real signed packages)
SERVE = {}
TAMPER_NAME = "evil.catpkg"


def build_serve_tree():
    lines = ["# remote test repo", "name|version|desc|file"]
    for fn in sorted(os.listdir(PKGSRC)):
        if not fn.endswith(".catpkg"):
            continue
        blob = open(os.path.join(PKGSRC, fn), "rb").read()
        name = blob[8:24].split(b"\0")[0].decode()
        ver = blob[24:40].split(b"\0")[0].decode()
        desc = blob[56:136].split(b"\0")[0].decode()
        lines.append("%s|%s|%s|%s" % (name, ver, desc, fn))
    SERVE["/repo/PKGS.TXT"] = ("\n".join(lines) + "\n").encode()
    for fn in lines[1:]:
        pass
    for fn in os.listdir(PKGSRC):
        if fn.endswith(".catpkg"):
            SERVE["/repo/" + fn] = open(os.path.join(PKGSRC, fn), "rb").read()
    # tampered copy: flip one payload byte of bc.catpkg
    bad = bytearray(SERVE["/repo/bc.catpkg"])
    bad[210] ^= 0xFF
    SERVE["/repo/" + TAMPER_NAME] = bytes(bad)
    # a text marker that only exists on the HOST server (proves real network)
    SERVE["/repo/HOST_MARKER.txt"] = b"CATOS_REMOTE_REPO_HOST_270\n"


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
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


def wait_for_text(s, needles, timeout=25.0):
    deadline = time.time() + timeout
    last = ""
    while time.time() < deadline:
        last = screen_text(s)
        if all(n in last for n in needles):
            return True, last
        time.sleep(0.5)
    return False, last


def main():
    iso = os.path.abspath(sys.argv[1] if len(sys.argv) > 1
                          else os.path.join(REPO, "kernel_v25", "catos.iso"))
    qemu = find_qemu(None)
    if not qemu:
        print("qemu-system-i386 not found")
        return 2

    build_serve_tree()
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", HTTP_PORT), Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    print("host http repo on 127.0.0.1:%d (%d objects)" % (HTTP_PORT, len(SERVE)))

    disk = os.path.join(HERE, "qemu_disk_pkgremote.img")
    if not os.path.exists(disk):
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
        type_text(s, "text", 0.05); sendkey(s, "ret", 0.6)
        sendkey(s, "y", 0.8); time.sleep(1.2)
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
        wait_for_text(s, ["dhcp ok"], 20.0)

        base = "http://10.0.2.2:%d/repo" % HTTP_PORT

        do("clear", 0.5)
        do("pkg repo %s" % base, 1.5)
        ok, txt = wait_for_text(s, ["remote repo ->"], 10.0)
        step("pkg repo persists base URL", ok, txt[-120:] if not ok else "")

        do("clear", 0.5)
        do("pkg repo", 1.2)
        ok, txt = wait_for_text(s, [base], 10.0)
        step("pkg repo shows saved URL", ok, txt[-120:] if not ok else "")

        do("clear", 0.5)
        do("pkg update", 5.0)
        ok, txt = wait_for_text(s, ["index updated"], 30.0)
        step("pkg update downloads remote index", ok, txt[-160:] if not ok else "")

        # prove real network: fetch the host-only marker via curl
        do("clear", 0.5)
        do("curl %s/HOST_MARKER.txt" % base, 4.0)
        ok, txt = wait_for_text(s, ["CATOS_REMOTE_REPO_HOST_270"], 30.0)
        step("HTTP GET reaches host repo (marker)", ok, txt[-160:] if not ok else "")

        # remove local bc first so the installed copy must come from network
        do("pkg remove desktop", 1.5)
        do("clear", 0.5)
        do("pkg fetch desktop", 6.0)
        ok, txt = wait_for_text(s, ["verifying", "installed desktop"], 40.0)
        step("pkg fetch downloads + verifies + installs", ok, txt[-200:] if not ok else "")

        do("clear", 0.5)
        do("desktop", 3.0)
        ok, txt = wait_for_text(s, ["Start", "Terminal]"], 20.0)
        step("remotely installed desktop runs", ok, txt[-160:] if not ok else "")
        sendkey(s, "q", 1.0)   # exit desktop loop

        # tampered package must be rejected
        do("clear", 0.5)
        do("pkg fetch %s/%s" % (base, TAMPER_NAME), 6.0)
        ok, txt = wait_for_text(s, ["SIGNATURE VERIFICATION FAILED"], 40.0)
        step("tampered package rejected by verifier", ok, txt[-200:] if not ok else "")
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
