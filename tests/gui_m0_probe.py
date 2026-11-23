#!/usr/bin/env python3
"""gui_m0_probe.py -- M0 骨架冒烟探针。

流程：启动 -> force_text 回到可读文本控制台 -> 输入 `gui` 进入图形块桌面
-> screendump 存 ppm -> Esc 退回 -> 校验提示条回来、help 含 gui、ls 正常。
串口全量落 artifacts/gui-blocks/m0-serial.log。

用法：python3 tests/gui_m0_probe.py [catos.iso]
"""
import os
import re
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qemu_driver import (find_qemu, kill_qemu, screen_text, sendkey,
                         type_text, wait_for_prompt, force_text, cmd)

REPO = os.path.dirname(HERE)
ART = os.path.join(REPO, "artifacts", "gui-blocks")
os.makedirs(ART, exist_ok=True)
ISO = os.path.join(REPO, "kernel_v25", "catos.iso")
PORT = 4471


def main():
    iso = sys.argv[1] if len(sys.argv) > 1 else ISO
    qemu = find_qemu()
    if not qemu:
        print("FAIL: qemu-system-i386 not found")
        return 2
    serial = os.path.join(ART, "m0-serial.log")
    if os.path.exists(serial):
        os.remove(serial)

    proc = subprocess.Popen(
        [qemu, "-m", "256", "-no-reboot", "-net", "none",
         "-cdrom", iso, "-boot", "order=d", "-display", "none",
         "-monitor", "tcp:127.0.0.1:%d,server,nowait" % PORT,
         "-vga", "vmware", "-serial", "file:" + serial],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    sock = None
    for _ in range(40):
        if proc.poll() is not None:
            print("FAIL: qemu exited rc=%r" % proc.returncode)
            return 2
        try:
            sock = socket.create_connection(("127.0.0.1", PORT), timeout=5)
            break
        except OSError:
            time.sleep(2)
    if sock is None:
        kill_qemu(proc)
        print("FAIL: no monitor")
        return 2

    try:
        force_text(sock)
        if not wait_for_prompt(sock, timeout=120.0):
            print("FAIL: shell prompt never appeared")
            return 3

        # 进入图形块桌面
        type_text(sock, "gui")
        sendkey(sock, "ret", 1.0)
        time.sleep(3.0)

        ppm = os.path.join(ART, "m0-desktop.ppm")
        if os.path.exists(ppm):
            os.remove(ppm)
        cmd(sock, "screendump " + ppm, wait=1.0)
        time.sleep(0.5)

        # Esc 干净退回文本控制台
        sendkey(sock, "esc", 1.0)
        time.sleep(2.0)
        txt = screen_text(sock)
        back = bool(re.search(r"catos:[^\n]*[\$#]", txt))

        type_text(sock, "help")
        sendkey(sock, "ret", 1.0)
        time.sleep(1.5)
        help_txt = screen_text(sock)

        type_text(sock, "ls")
        sendkey(sock, "ret", 1.5)
        ls_txt = screen_text(sock)

        print("BACK_TO_TEXT=%s" % back)
        print("HELP_HAS_GUI=%s" % ("gui" in help_txt))
        print("LS_PROMPT_OK=%s" % bool(re.search(r"catos:[^\n]*[\$#]", ls_txt)))
        print("PPM_EXISTS=%s" % os.path.exists(ppm))
        print("ART=" + ART)
        return 0 if back and "gui" in help_txt else 1
    finally:
        kill_qemu(proc)


if __name__ == "__main__":
    sys.exit(main())
