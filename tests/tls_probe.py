#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tls_probe.py -- D2 (HTTPS/TLS 1.2) end-to-end acceptance for CatOS.

Boots a CatOS guest behind QEMU slirp user-net, serves a fixed marker over a
TLS 1.2 server pinned to the 0xC027 cipher
(TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256), and verifies the guest reaches it
with `curl https://10.0.2.2:8443/`, then dumps a screendump PNG.

Acceptance contract (D2):
  * guest screen contains the fixed marker ``CATOS_TLS_OK_278``.
  * evidence screenshot is written under artifacts/d2-tls/.

Notes / honesty:
  * The v1 client performs encryption only -- NO certificate chain / hostname /
    RSA signature verification (MITM-exposed).  The server certificate here is
    a throwaway self-signed cert; the client reads and discards the SKE
    signature.
  * Only TLS 1.2, suite 0xC027, secp256r1, no session resumption, no SNI,
    single blocking connection.

usage:
    python tests/tls_probe.py [catos.iso]
"""
import http.server
import os
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from qemu_driver import (  # noqa: E402
    find_qemu, kill_qemu, screen_text, sendkey, type_text,
    wait_for_prompt, force_text, cmd,
)

MONITOR_PORT = 4501
TLS_PORT = 8443
MARKER = "CATOS_TLS_OK_278"
ARTIFACT_DIR = os.path.join(REPO, "artifacts", "d2-tls")


# --------------------------------------------------------------------------
# Host-side TLS 1.2 server (pinned to the 0xC027 cipher), serving the marker.
# --------------------------------------------------------------------------

class MarkerHandler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = MARKER.encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


def make_self_signed(cert_dir):
    """Generate a throwaway RSA-2048 self-signed cert/key for the test server."""
    key = os.path.join(cert_dir, "d2.key")
    cert = os.path.join(cert_dir, "d2.crt")
    subprocess.run(
        ["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
         "-keyout", key, "-out", cert, "-subj", "/CN=10.0.2.2", "-days", "2"],
        check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    return cert, key


def start_tls_server(cert, key):
    httpd = http.server.ThreadingHTTPServer(("127.0.0.1", TLS_PORT),
                                           MarkerHandler)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(cert, key)
    # 0xC027 = TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256, TLS 1.2 only.
    ctx.set_ciphers("ECDHE-RSA-AES128-SHA256")
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    ctx.maximum_version = ssl.TLSVersion.TLSv1_2
    httpd.socket = ctx.wrap_socket(httpd.socket, server_side=True)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    return httpd


# --------------------------------------------------------------------------
# Guest interaction helpers
# --------------------------------------------------------------------------

def wait_for_text(sock, needles, timeout=30.0):
    deadline = time.time() + timeout
    last = ""
    while time.time() < deadline:
        last = screen_text(sock)
        if all(n in last for n in needles):
            return True, last
        time.sleep(0.5)
    return False, last


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

    # Host TLS server + throwaway cert.
    cert_dir = tempfile.mkdtemp(prefix="catos_d2_tls_")
    cert, key = make_self_signed(cert_dir)
    httpd = start_tls_server(cert, key)
    print("host TLS 1.2 server on 127.0.0.1:%d (cipher 0xC027)" % TLS_PORT)

    # QEMU guest: pcnet behind slirp, monitor on 4501.
    from qemu_driver import _stage_image
    iso_i = _stage_image(iso)
    cmdline = [
        qemu, "-m", "256", "-no-reboot", "-display", "none",
        "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
        "-monitor", "tcp:127.0.0.1:%d,server,nowait" % MONITOR_PORT,
        "-cdrom", iso_i, "-boot", "order=d",
    ]
    proc = subprocess.Popen(cmdline, stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL)

    sock = None
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
            print("FATAL: no monitor on port %d" % MONITOR_PORT)
            return 3

        # Boot -> text console -> shell prompt.
        force_text(sock, settle=18.0)
        if not wait_for_prompt(sock, timeout=120.0):
            print("FATAL: shell prompt never appeared")
            return 3

        # Network config: DHCP first, static slirp defaults as fallback.
        send_line(sock, "netcfg dhcp", 3.0)
        ok, txt = wait_for_text(sock, ["dhcp ok"], 15.0)
        net_mode = "dhcp"
        if not ok:
            print("DHCP did not lease; falling back to static slirp defaults")
            send_line(sock, "netcfg 10.0.2.15 10.0.2.2 10.0.2.3", 2.0)
            net_mode = "static"
        print("network config mode: %s" % net_mode)

        # The actual HTTPS fetch under test.
        send_line(sock, "clear", 0.8)
        send_line(sock, "curl https://10.0.2.2:%d/" % TLS_PORT, 4.0)
        ok, txt = wait_for_text(sock, [MARKER], 30.0)
        print("guest screen tail: %s" % txt[-160:])

        # Screendump evidence (PPM via monitor, converted to PNG).
        ppm = os.path.join(ARTIFACT_DIR, "tls_probe.ppm")
        png = os.path.join(ARTIFACT_DIR, "tls_probe.png")
        cmd(sock, "screendump %s" % ppm, 2.0)
        try:
            from PIL import Image
            if os.path.exists(ppm):
                Image.open(ppm).save(png)
                print("screenshot -> %s" % png)
        except Exception as e:
            print("PNG conversion skipped: %s" % e)

        if not ok:
            print("FAIL: marker %r not on guest screen" % MARKER)
            return 1
        print("PASS: guest fetched %s over TLS 1.2" % MARKER)
        return 0
    finally:
        kill_qemu(proc)
        try:
            httpd.shutdown()
        except Exception:
            pass


if __name__ == "__main__":
    sys.exit(main())
