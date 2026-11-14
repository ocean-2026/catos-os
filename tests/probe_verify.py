#!/usr/bin/env python3
"""独立探针验证：启动 catos.iso，在 shell 里跑三个探针，用 QEMU monitor 的
xp 命令直接读 VGA 文本缓冲区 0xB8000 还原屏幕文字（无需 OCR）。"""
import os, re, socket, subprocess, sys, time

QEMU = r"C:\qemu-portable\qemu-system-i386.exe"
ISO = sys.argv[1] if len(sys.argv) > 1 else "kernel_v25/catos.iso"
MON = ("127.0.0.1", 4455)

def start():
    p = subprocess.Popen([QEMU, "-m", "256", "-no-reboot", "-net", "none",
                          "-cdrom", ISO, "-boot", "order=d", "-display", "none",
                          "-monitor", "tcp:%s:%d,server,nowait" % MON],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(25):
        try:
            s = socket.create_connection(MON, timeout=5)
            return p, s
        except OSError:
            time.sleep(2)
    raise RuntimeError("monitor unreachable")

def cmd(s, c, wait=0.6):
    s.sendall((c + "\n").encode())
    time.sleep(wait)
    s.setblocking(False)
    time.sleep(0.15)
    try:
        r = s.recv(262144)
    except Exception:
        r = b""
    s.setblocking(True)
    return r.decode(errors="replace")

def screen_text(s):
    """读 VGA 文本缓冲区 0xB8000（80x25，属性字节在高位）。"""
    out = cmd(s, "xp/4000xb 0xb8000", wait=1.2)
    hexes = re.findall(r"0x([0-9a-fA-F]{2})", out)
    b = bytes(int(h, 16) for h in hexes)
    rows = []
    for r in range(25):
        row = ""
        for c in range(80):
            ch = b[(r * 80 + c) * 2] if (r * 80 + c) * 2 < len(b) else 32
            row += chr(ch) if 32 <= ch < 127 else (" " if ch in (0, 32) else ".")
        rows.append(row.rstrip())
    return "\n".join(rows)

def type_line(s, text):
    for ch in text:
        key = {" ": "spc", "-": "minus", ".": "dot", "/": "slash",
               "_": "shift-minus", ":": "shift-semicolon"}.get(ch)
        if key is None:
            key = "shift-%s" % ch.lower() if ch.isupper() else ch
        cmd(s, "sendkey " + key, 0.12)
    cmd(s, "sendkey ret", 0.5)

def main():
    p, s = start()
    time.sleep(28)                      # TCG 下启动较慢
    print("=== BOOT SCREEN ===")
    print(screen_text(s))
    for cmdline, label in [("run /bin/allocprobe.ce", "ALLOCPROBE"),
                           ("run /bin/heaptest.ce", "HEAPTEST"),
                           ("run /bin/go.ce", "GO")]:
        print("\n=== %s : %s ===" % (label, cmdline))
        type_line(s, cmdline)
        time.sleep(4)
        print(screen_text(s))
    cmd(s, "quit", 0.3)
    try:
        p.wait(timeout=5)
    except Exception:
        p.kill()

main()
