#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""审美审一轮：FM 标题条分列修复后重出图。ISO=新构建 catos.iso。"""
import os, sys, time, subprocess, socket

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, "tests"))
import qemu_driver as q

ISO = os.path.join(REPO, "kernel_v25", "catos.iso")
OUT = HERE
PORT = 4899


def boot_serial(serial):
    qemu = q.find_qemu() or os.path.expanduser("~/bin/qemu-system-i386")
    cmd = [qemu, "-m", "256", "-smp", "1", "-cpu", "qemu32",
           "-vga", "std", "-no-reboot", "-net", "none", "-display", "none",
           "-boot", "order=d", "-cdrom", ISO,
           "-serial", "file:" + serial,
           "-monitor", "tcp:127.0.0.1:%d,server,nowait" % PORT]
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(40):
        if proc.poll() is not None:
            raise RuntimeError("qemu exited rc=%r" % proc.returncode)
        try:
            return proc, socket.create_connection(("127.0.0.1", PORT), timeout=5)
        except OSError:
            time.sleep(2)
    q.kill_qemu(proc)
    raise RuntimeError("no monitor")


def shoot(s, png):
    ppm = png.replace(".png", ".ppm")
    if os.path.exists(ppm):
        os.remove(ppm)
    q.cmd(s, "screendump " + ppm, 1.0)
    dl = time.time() + 10
    while time.time() < dl:
        if os.path.exists(ppm) and os.path.getsize(ppm) > 0:
            break
        time.sleep(0.3)
    from PIL import Image
    Image.open(ppm).convert("RGB").save(png)
    os.remove(ppm)


def wait_marker(serial, marker, timeout=25.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            txt = open(serial, encoding="utf-8", errors="replace").read()
            if marker in txt:
                return txt
        except OSError:
            pass
        time.sleep(0.3)
    return open(serial, encoding="utf-8", errors="replace").read()


def wait_ready_serial(serial, timeout=90.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            if "shell prompt ready" in open(serial, encoding="utf-8", errors="replace").read():
                return True
        except OSError:
            pass
        time.sleep(1.0)
    return False


def run(name, marker, gui_cmd, shot):
    serial = os.path.join(OUT, "serial-aes-" + name + ".log")
    if os.path.exists(serial):
        os.remove(serial)
    proc, s = boot_serial(serial)
    try:
        if not wait_ready_serial(serial):
            raise RuntimeError("no prompt ready")
        time.sleep(2.5)
        q.type_text(s, "lang zh", 0.05)
        q.sendkey(s, "ret", 1.0)
        time.sleep(1.0)
        q.type_text(s, gui_cmd, 0.05)
        q.sendkey(s, "ret", 1.0)
        txt = wait_marker(serial, marker)
        shoot(s, os.path.join(OUT, shot))
        for l in [x.strip() for x in txt.splitlines() if marker.split()[0] in x][:8]:
            print("   ", l)
        print("[%s] -> %s" % (gui_cmd, shot))
    finally:
        q.kill_qemu(proc)
        time.sleep(1)


run("fm", "MW:fm", "gui mw fm", "aes-fix-fm.png")
run("tb", "TB:entry", "gui mw tb", "aes-fix-tb.png")
