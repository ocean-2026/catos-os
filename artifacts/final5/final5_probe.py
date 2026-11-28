#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""final5 离屏合成 / 拖动残影取证探针。

逐场景起 QEMU（固定 -vga std，HMP screendump 取新鲜前台缓冲）：
  - offscr: 默认块桌面帧，串口留 `GUI:shadow mode=1 ...` 离屏影子分配口径。
  - cursor: HMP 相对移动鼠标两帧，留光标位置变化截图。
  - drag:   既有内核注入拖拽场景（shot-drag / free-interact / crop 出边），
            抓拖拽后帧，核对旧位置回底色、无残影。

只做取证与串口打点采集；不修改内核行为。
"""
import os, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, "tests"))
import qemu_driver as q

ISO = os.path.join(REPO, "kernel_v25", "catos.iso")
DISK = os.path.join(REPO, "tests", "qemu_disk_vprobe.img")
ART = HERE
PORT_BASE = 4740


def shoot(s, png, wait=2.0):
    ppm = png.replace(".png", ".ppm")
    if os.path.exists(ppm):
        os.remove(ppm)
    q.cmd(s, "screendump " + ppm, 1.0)
    deadline = time.time() + 10.0
    while time.time() < deadline:
        if os.path.exists(ppm) and os.path.getsize(ppm) > 0:
            break
        time.sleep(0.3)
    try:
        from PIL import Image
        Image.open(ppm).convert("RGB").save(png)
        print("SHOT", os.path.relpath(png, ART))
    except Exception as e:
        print("PNG convert fail", e)


def boot_one(serial, port):
    qemu = q.find_qemu()
    proc, s = q.boot(qemu, ISO, port, disk=DISK,
                      extra_args=["-smp", "1", "-cpu", "qemu32", "-vga", "std",
                                  "-serial", "file:" + serial])
    q.force_text(s, settle=10)
    if not q.wait_for_prompt(s, timeout=90):
        raise RuntimeError("no prompt")
    return proc, s


def run(cmd, serial, shots, settle=6.0, mouse=None):
    proc, s = boot_one(serial, PORT_BASE + run._n)
    run._n += 1
    try:
        if cmd:
            q.type_text(s, cmd, 0.05)
            q.sendkey(s, "ret", 1.0)
            time.sleep(settle)
        if mouse:
            for mv in mouse:
                q.cmd(s, "mouse_move %d %d" % mv, 0.3)
                time.sleep(0.6)
        for png in shots:
            shoot(s, os.path.join(ART, png))
        time.sleep(0.5)
    finally:
        q.kill_qemu(proc)
        time.sleep(1)


run._n = 0


def main():
    only = set(sys.argv[1:]) or {"offscr", "cursor", "drag"}

    if "offscr" in only:
        run("gui", os.path.join(ART, "offscr", "serial-offscr.log"),
            [os.path.join("offscr", "offscr-shadow-01.png")], settle=6.0)
        run("gui", os.path.join(ART, "offscr", "serial-offscr-2.log"),
            [os.path.join("offscr", "offscr-shadow-02.png")], settle=4.0)

    if "cursor" in only:
        run("gui", os.path.join(ART, "cursor", "serial-cursor.log"),
            [os.path.join("cursor", "cursor-move-a.png")],
            settle=5.0, mouse=[(120, 80)])
        run("gui", os.path.join(ART, "cursor", "serial-cursor-2.log"),
            [os.path.join("cursor", "cursor-move-b.png")],
            settle=5.0, mouse=[(200, 120)])

    if "drag" in only:
        # shot-drag: A 自旧矩形拖到右侧，旧位回底色。
        run("gui shot-drag", os.path.join(ART, "drag", "serial-drag-shot.log"),
            [os.path.join("drag", "drag-slow-01.png")], settle=4.0)
        run("gui shot-drag", os.path.join(ART, "drag", "serial-drag-shot-2.log"),
            [os.path.join("drag", "drag-slow-02.png")], settle=4.0)
        run("gui shot-drag", os.path.join(ART, "drag", "serial-drag-shot-3.log"),
            [os.path.join("drag", "drag-slow-03.png")], settle=4.0)
        # free-interact: 标题带横向拖拽（窗口内子块）。
        run("gui shot-free-interact",
            os.path.join(ART, "drag", "serial-drag-interact.log"),
            [os.path.join("drag", "drag-fling-01.png")], settle=4.0)
        run("gui shot-free-interact",
            os.path.join(ART, "drag", "serial-drag-interact-2.log"),
            [os.path.join("drag", "drag-fling-02.png")], settle=4.0)
        run("gui shot-free-interact",
            os.path.join(ART, "drag", "serial-drag-interact-3.log"),
            [os.path.join("drag", "drag-fling-03.png")], settle=4.0)
        # crop: 子块出屏边裁剪场景，半出屏再拖回的边界对照。
        run("gui shot-crop", os.path.join(ART, "drag", "serial-drag-crop.log"),
            [os.path.join("drag", "drag-edge-out-back.png")], settle=4.0)


if __name__ == "__main__":
    main()
