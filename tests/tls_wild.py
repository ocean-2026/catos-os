#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tls_wild.py -- CatOS TLS client against real public hosts (wild-site check).

Boots a CatOS guest behind QEMU slirp user-net, brings the NIC up over DHCP,
then issues:

    curl https://example.com        (required gate: HTTP 200 + non-empty body)
    curl https://www.baidu.com      (secondary: recorded, not gate-fatal)

Per host the runner records:
  * HTTP status / body byte count parsed from the guest screen,
  * `TLS:rec msgs=`, `TLS:wild status=...` and any `TLS:alert`/`TLS:warn`
    lines captured from the guest serial console.

Evidence written under ``artifacts/d2-tls/``:
  * ``wild_result.json`` -- per-host status/bytes + serial evidence + verdict,
  * ``wild_serial.log``  -- captured TLS serial lines,
  * ``wild_<host>.ppm``  -- guest screendump after each curl.

This script does NOT stand up a local server; it depends on outbound network.
It reuses the shared qemu_driver; it does not change the local forced-handshake
contract in ``tests/tls_probe.py``.

usage:
    python tests/tls_wild.py [catos.iso]
"""
import json
import os
import re
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

# host -> (role, expected status). example.com is the required gate;
# baidu is a secondary real-site sample recorded for the overnight report.
HOSTS = [
    ("https://example.com", "required", 200),
    ("https://www.baidu.com", "secondary", 200),
]

RE_STATUS = re.compile(r"HTTP\s+(\d+)")
RE_BYTES = re.compile(r"---\s+(\d+)\s+bytes\s+---")


def send_line(sock, line, wait=1.5):
    type_text(sock, line)
    sendkey(sock, "ret", wait)


def parse_screen(txt):
    """Pull status / byte count out of the curl panel on the guest screen."""
    status = None
    m = RE_STATUS.search(txt)
    if m:
        status = int(m.group(1))
    n = None
    m = RE_BYTES.search(txt)
    if m:
        n = int(m.group(1))
    return status, n


def wait_curl_done(sock, timeout=25.0):
    """Poll until the curl panel prints a byte footer (or status line)."""
    deadline = time.time() + timeout
    last = ""
    while time.time() < deadline:
        last = screen_text(sock)
        if RE_BYTES.search(last) or RE_STATUS.search(last):
            return last
        time.sleep(0.5)
    return last


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
    report = {"iso": iso, "hosts": [], "serial": {}, "verdict": "FAIL"}
    try:
        proc, sock = boot(qemu, iso, MONITOR_PORT, extra_args=extra)
        force_text(sock, settle=18.0)
        if not wait_for_prompt(sock, timeout=120.0):
            print("FATAL: shell prompt never appeared")
            report["verdict"] = "FAIL"
            return 3

        send_line(sock, "netcfg dhcp", 3.0)
        ok, _ = _wait_text(sock, ["dhcp ok"], 15.0)
        if not ok:
            print("DHCP did not lease; trying static slirp defaults")
            send_line(sock, "netcfg 10.0.2.15 10.0.2.2 10.0.2.3", 2.0)

        for host, role, want_status in HOSTS:
            send_line(sock, "clear", 0.6)
            send_line(sock, "curl %s" % host, 4.0)
            txt = wait_curl_done(sock)
            status, nbytes = parse_screen(txt)
            print("=== screen after %s ===" % host)
            print(txt[-400:])
            ppm = os.path.join(
                ARTIFACT_DIR, "wild_%s.ppm" %
                host.split("//")[1].replace(".", "_"))
            cmd(sock, "screendump %s" % ppm, 2.0)

            ok = (status == want_status and nbytes is not None and nbytes > 0)
            entry = {
                "host": host, "role": role, "status": status,
                "bytes": nbytes, "expected_status": want_status,
                "verdict": "PASS" if ok else "FAIL",
                "shot": ppm,
            }
            report["hosts"].append(entry)
            print("%s -> status=%s bytes=%s %s" %
                  (host, status, nbytes, entry["verdict"]))

        # serial capture: TLS / alert / warn / wild lines
        serial_lines = []
        if os.path.exists(SERIAL_LOG):
            with open(SERIAL_LOG, "r", errors="replace") as f:
                serial_lines = [ln.rstrip() for ln in f
                                if any(k in ln for k in
                                       ("TLS:", "alert", "warn", "rec msgs"))]
        print("=== serial TLS lines ===")
        for ln in serial_lines[-40:]:
            print(ln)

        has_rec = any("TLS:rec msgs=" in ln for ln in serial_lines)
        has_wild = any("TLS:wild status=200" in ln for ln in serial_lines)
        report["serial"] = {
            "tls_rec_msgs": has_rec,
            "tls_wild_status_200": has_wild,
            "lines": serial_lines[-40:],
        }

        with open(os.path.join(ARTIFACT_DIR, "wild_serial.log"), "w") as f:
            f.write("\n".join(serial_lines))

        required = [h for h in report["hosts"] if h["role"] == "required"]
        req_ok = all(h["verdict"] == "PASS" for h in required)
        report["verdict"] = "PASS" if (req_ok and has_rec and has_wild) else "FAIL"

        with open(os.path.join(ARTIFACT_DIR, "wild_result.json"), "w") as f:
            json.dump(report, f, ensure_ascii=False, indent=2)

        print("wild verdict: %s (rec_msgs=%s wild_200=%s)" %
              (report["verdict"], has_rec, has_wild))
        return 0 if report["verdict"] == "PASS" else 1
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
