#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tls_wild.py -- CatOS TLS client against real public hosts (wild-site check).

Boots a CatOS guest behind QEMU slirp user-net, brings the NIC up over DHCP,
then issues:

    curl https://example.com
    curl https://www.baidu.com

and records, per host:
  * HTTP status / body byte count shown on the guest screen,
  * any `TLS:rec msgs=`, `TLS:alert`, `TLS:warn` lines captured from the
    guest serial console.

This script does NOT stand up a local server; it depends on outbound network.

usage:
    python tests/tls_wild.py [catos.iso]
"""
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from qemu_driver import (  # noqa: E402
    find_qemu, kill_qemu, screen_text, wait_for_prompt,
    force_text, cmd, sendkey, type_text,
)

MONITOR_PORT = 4510
SERIAL_LOG = "/tmp/catos_wild_serial.log"
ARTIFACT_DIR = os.path.join(REPO, "artifacts", "d2-tls")
HOSTS = ["https://example.com", "https://www.baidu.com"]


def send_line(sock, line, wait=1.5):
    type_text(sock, line)
    sendkey(sock, "ret", wait)


def main():
    iso = os.path.abspath(sys.argv[1] if len(sys.argv) > 1
                          else os.path.join(REPO, "kernel_v25", "catos.iso"))
    qemu = find_qemu(None)
    if not qemu:
        print("FATAL: qemu-system-i386 not found")
        return 2

    os.makedirs(ARTIFACT_DIR, exist_ok=True)
    if os.path.exists(SERIAL_LOG):
        os.remove(SERIAL_LOG)

    extra = [
        "-netdev", "user,id=n0",
        "-device", "e1000,netdev=n0",
        "-serial", "file:%s" % SERIAL_LOG,
    ]
    from qemu_driver import boot
    proc = None
    sock = None
    try:
        proc, sock = boot(qemu, iso, MONITOR_PORT, extra_args=extra)
        force_text(sock, settle=18.0)
        if not wait_for_prompt(sock, timeout=120.0):
            print("FATAL: shell prompt never appeared")
            return 3

        send_line(sock, "netcfg dhcp", 3.0)
        ok, _ = _wait_text(sock, ["dhcp ok"], 15.0)
        if not ok:
            print("DHCP did not lease; trying static slirp defaults")
            send_line(sock, "netcfg 10.0.2.15 10.0.2.2 10.0.2.3", 2.0)

        results = {}
        for host in HOSTS:
            send_line(sock, "clear", 0.6)
            send_line(sock, "curl %s" % host, 4.0)
            time.sleep(6.0)
            txt = screen_text(sock)
            results[host] = txt
            print("=== screen after %s ===" % host)
            print(txt[-400:])
            ppm = os.path.join(ARTIFACT_DIR, "wild_%s.ppm" %
                               host.split("//")[1].replace(".", "_"))
            cmd(sock, "screendump %s" % ppm, 2.0)

        # serial capture: TLS / alert / warn lines
        serial_lines = []
        if os.path.exists(SERIAL_LOG):
            with open(SERIAL_LOG, "r", errors="replace") as f:
                serial_lines = [ln.rstrip() for ln in f
                                if any(k in ln for k in
                                       ("TLS:", "alert", "warn", "rec msgs"))]
        print("=== serial TLS lines ===")
        for ln in serial_lines[-40:]:
            print(ln)
        with open(os.path.join(ARTIFACT_DIR, "wild_serial.log"), "w") as f:
            f.write("\n".join(serial_lines))
        return 0
    finally:
        if proc is not None:
            kill_qemu(proc)


def _wait_text(sock, needles, timeout):
    deadline = time.time() + timeout
    last = ""
    while time.time() < deadline:
        last = screen_text(sock)
        if all(n in last for n in needles):
            return True, last
        time.sleep(0.5)
    return False, last


if __name__ == "__main__":
    sys.exit(main())
