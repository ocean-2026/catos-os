#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""net_dhcp_probe.py -- v2.66 DHCP client acceptance.

Boots CatOS with an e1000 NIC behind slirp user-net (which runs a DHCP
server, exactly like VMware NAT / VMnet8 does) and verifies:

  1. `netcfg dhcp` performs DISCOVER->OFFER->REQUEST->ACK and prints
     "dhcp ok" with the slirp lease 10.0.2.15 / gw 10.0.2.2 / dns 10.0.2.3.
  2. `ping 10.0.2.2` still replies with the DHCP-acquired address.
  3. `curl http://10.0.2.2:PORT/probe.txt` fetches a file served on the
     HOST (slirp maps 10.0.2.2 -> host loopback) -- the full
     DHCP -> ARP -> TCP -> HTTP chain.

usage: python tests/net_dhcp_probe.py [catos.iso]
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
                         wait_for_prompt)  # noqa: E402

REPO = os.path.dirname(HERE)
MONITOR_PORT = 4494
HTTP_PORT = 8123
PROBE_BODY = "CATOS_DHCP_OK_266"


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = PROBE_BODY.encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *a):
        pass


def net_args():
    return ["-netdev", "user,id=n0", "-device", "e1000,netdev=n0"]


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

    srv = http.server.ThreadingHTTPServer(("127.0.0.1", HTTP_PORT), Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    print("host http server on 127.0.0.1:%d" % HTTP_PORT)

    from qemu_driver import _stage_image, read_screen
    disk = os.path.abspath(os.path.join(HERE, "qemu_disk_dhcp.img"))
    if not os.path.exists(disk):
        with open(disk, "wb") as fh:
            fh.truncate(64 * 1024 * 1024)
    iso_i = _stage_image(iso)
    disk_i = _stage_image(disk)
    # disk MUST be ide index 0 (fat_probe hardcodes drive 0), ISO index 1
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
        print("%-4s %-40s %s" % ("PASS" if ok else "FAIL", name, note))

    def do(line, wait=1.5):
        type_text(s, line)
        sendkey(s, "ret", wait)

    try:
        # Graphics mode is the boot default; blindly switch to text before
        # reading the VGA text buffer (the monitor cannot see the framebuffer).
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

        do("clear", 1.0)
        do("netcfg dhcp", 3.0)
        ok, txt = wait_for_text(s, ["dhcp ok", "10.0.2.15"], 20.0)
        step("netcfg dhcp: lease 10.0.2.15 acquired", ok, txt[-120:] if not ok else "")
        step("netcfg dhcp: gw/dns from server",
             "10.0.2.2" in txt and "10.0.2.3" in txt,
             "" if ok else "gw/dns missing")

        do("clear", 1.0)
        do("netcfg", 1.5)
        ok, txt = wait_for_text(s, ["[dhcp]"], 10.0)
        step("netcfg: config source shows [dhcp]", ok)

        do("clear", 1.0)
        do("ping 10.0.2.2", 3.0)
        ok, txt = wait_for_text(s, ["reply"], 30.0)
        step("ping 10.0.2.2 with DHCP address", ok, txt[-120:] if not ok else "")

        do("clear", 1.0)
        do("curl http://10.0.2.2:%d/probe.txt" % HTTP_PORT, 4.0)
        ok, txt = wait_for_text(s, [PROBE_BODY], 30.0)
        step("curl: HTTP GET via host loopback", ok, txt[-120:] if not ok else "")
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
