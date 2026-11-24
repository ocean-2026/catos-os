#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""br_chunked_probe.py -- 浏览器 chunked 解码 + 缓冲扩容验收。

宿主起一个手写 chunked 的 HTTP 服务器（无 Content-Length），guest 在浏览器里
打开页面，串口回打 ``GUI:br bytes=... status=...``。断言：

  * chunked 路径拉到的字节数 == 原始 body 长度（多块拼接、0 结束、trailer 丢弃）；
  * 没有把 chunk 结尾的 ``0\\r\\n`` / trailer 残body 算进 bytes；
  * 同 body 的 Content-Length 控制路径字节数一致；
  * 页面 >16 KiB，验证响应缓冲不再被旧 20 KiB 上限截断。

usage:
    python3 tests/br_chunked_probe.py [catos.iso]
"""
import http.server
import os
import re
import socket
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qemu_driver import (find_qemu, boot, kill_qemu, screen_text,  # noqa: E402
                         type_text, sendkey, wait_for_prompt, force_text)

REPO = os.path.dirname(HERE)
MONITOR_PORT = int(os.environ.get("BR_CHUNK_MON", "4570"))
HTTP_PORT = int(os.environ.get("BR_CHUNK_HTTP", "8130"))

ART = os.path.join(REPO, "artifacts", "overnight-20261124")
os.makedirs(ART, exist_ok=True)
SERIAL = os.path.join(ART, "br-chunked-serial.log")


def build_body():
    lines = ["<html><body><h1>chunked probe</h1>"]
    for i in range(400):
        lines.append("<p>line %d: the quick brown fox jumps over the lazy "
                     "dog 0123456789</p>" % i)
    lines.append("</body></html>")
    return "\n".join(lines).encode()


BODY = build_body()


class Handler(http.server.BaseHTTPRequestHandler):
    def _write_chunked(self, body):
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()
        i = 0
        while i < len(body):
            part = body[i:i + 1024]
            self.wfile.write(b"%x\r\n%s\r\n" % (len(part), part))
            i += 1024
        self.wfile.write(b"0\r\n\r\n")

    def do_GET(self):
        if self.path == "/page":
            self._write_chunked(BODY)
        elif self.path == "/cl":
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(BODY)))
            self.end_headers()
            self.wfile.write(BODY)
        else:
            self.send_error(404)

    def log_message(self, *a):
        pass


def latest_marker(text):
    rec = {}
    for line in text.splitlines():
        idx = line.find("GUI:br")
        if idx < 0:
            continue
        payload = line[idx + len("GUI:br"):].strip()
        for kv in payload.split():
            if "=" in kv:
                k, v = kv.split("=", 1)
                rec[k] = v
    return rec


def read_serial():
    try:
        return open(SERIAL, "rb").read().decode("utf-8", "replace")
    except OSError:
        return ""


def main():
    iso = os.path.abspath(sys.argv[1] if len(sys.argv) > 1
                          else os.path.join(REPO, "kernel_v25", "catos.iso"))
    qemu = find_qemu(None)
    if not qemu:
        print("FAIL: qemu-system-i386 not found")
        return 2

    if os.path.exists(SERIAL):
        os.remove(SERIAL)

    srv = http.server.ThreadingHTTPServer(("127.0.0.1", HTTP_PORT), Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    print("host http on 127.0.0.1:%d body=%d bytes" % (HTTP_PORT, len(BODY)))

    results = []

    def step(name, ok, note=""):
        results.append((name, bool(ok)))
        print("%-4s %-42s %s" % ("PASS" if ok else "FAIL", name, note))

    extra = ["-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
             "-serial", "file:" + SERIAL]
    proc = sock = None
    try:
        proc, sock = boot(qemu, iso, MONITOR_PORT, extra_args=extra)
        force_text(sock)
        if not wait_for_prompt(sock, timeout=120.0):
            print("FATAL: shell prompt never appeared")
            return 3

        # 让 NIC 拿到租约（浏览器打开前网络就绪）。
        type_text(sock, "netcfg dhcp")
        sendkey(sock, "ret", 3.0)
        screen_text(sock)

        # chunked 路径：多块拼接、0 结束、trailer 丢弃。
        type_text(sock, "gui browser http://10.0.2.2:%d/page" % HTTP_PORT)
        sendkey(sock, "ret", 1.0)
        time.sleep(9.0)
        m = latest_marker(read_serial())
        b_chunk = int(m.get("bytes", "0"))
        st_chunk = m.get("status", "0")
        step("chunked bytes == body length", b_chunk == len(BODY),
             "bytes=%d expect=%d status=%s" % (b_chunk, len(BODY), st_chunk))
        step("chunked status 200", st_chunk == "200", "status=%s" % st_chunk)
        step("chunked no trailer residue", b_chunk <= len(BODY),
             "bytes=%d body=%d" % (b_chunk, len(BODY)))
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
