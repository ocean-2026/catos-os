#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""One-off: boot guest, curl the local TLS relay (10.0.2.2:8444)."""
import os, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE); sys.path.insert(0, HERE)
from qemu_driver import (find_qemu, kill_qemu, screen_text, wait_for_prompt,
                         force_text, cmd, sendkey, type_text, boot)

PORT = 4511
URL = sys.argv[1] if len(sys.argv) > 1 else "https://10.0.2.2:8444/"

def send_line(s, line, wait=1.5):
    type_text(s, line); sendkey(s, "ret", wait)

def main():
    iso = os.path.join(REPO, "kernel_v25", "catos.iso")
    qemu = find_qemu(None)
    proc=None; sock=None
    extra=["-netdev","user,id=n0","-device","e1000,netdev=n0"]
    try:
        proc,sock=boot(qemu, iso, PORT, extra_args=extra)
        force_text(sock, settle=18.0)
        if not wait_for_prompt(sock, timeout=120.0):
            print("FATAL prompt"); return 3
        send_line(sock, "netcfg dhcp", 3.0)
        time.sleep(2)
        send_line(sock, "clear", 0.6)
        send_line(sock, "curl %s" % URL, 4.0)
        time.sleep(6)
        print("=== screen ==="); print(screen_text(sock)[-300:])
        return 0
    finally:
        if proc: kill_qemu(proc)

if __name__=="__main__":
    sys.exit(main())
