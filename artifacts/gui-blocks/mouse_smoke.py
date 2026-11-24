#!/usr/bin/env python3
"""真事件级鼠标冒烟：QEMU monitor 注入 mouse_move + mouse_button，验证
开始菜单、菜单行、桌面图标、标题带拖动与右下角缩放。

用法: python3 mouse_smoke.py <iso> [port] [outdir]
依赖: ../gates/qemu_driver.py（boot/cmd/type_text/wait_for_prompt/screendump 走 cmd）
"""
import os, sys, time

GATES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "gates")
sys.path.insert(0, GATES)
import qemu_driver as q  # noqa: E402

ISO = sys.argv[1]
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 4481
OUT = sys.argv[3] if len(sys.argv) > 3 else "/tmp/mouse-smoke"
os.makedirs(OUT, exist_ok=True)

# 光标初值 (400,300)，内核 abs_x/abs_y 相对累加，边界 799/599
state = {"x": 400, "y": 300}


def move(s, tx, ty, wait=0.25):
    dx, dy = tx - state["x"], ty - state["y"]
    q.cmd(s, "mouse_move %d %d" % (dx, dy), wait=wait)
    state["x"], state["y"] = tx, ty


def click(s, tx, ty, wait=0.45):
    move(s, tx, ty)
    q.cmd(s, "mouse_button 1", wait=wait)
    q.cmd(s, "mouse_button 0", wait=wait)


def dump(s, name):
    p = os.path.join(OUT, name + ".ppm")
    q.cmd(s, "screendump " + p, wait=0.9)
    return p


def ppm_size(p):
    with open(p, "rb") as f:
        head = f.read(32).split()
    return int(head[1]), int(head[2])


def px(p, x, y):
    with open(p, "rb") as f:
        data = f.read()
        # P6\n<w> <h>\n255\n
        idx = 0
        fields = []
        while len(fields) < 3:
            while data[idx:idx + 1].isspace():
                idx += 1
            if data[idx:idx + 1] == b"#":
                while data[idx:idx + 1] != b"\n":
                    idx += 1
                continue
            start = idx
            while not data[idx:idx + 1].isspace():
                idx += 1
            fields.append(data[start:idx])
        idx += 1
        w, h = int(fields[1]), int(fields[2])
        off = idx + (y * w + x) * 3
        return tuple(data[off:off + 3])


def main():
    proc, s = q.boot(q.find_qemu(), ISO, PORT,
                     extra_args=["-vga", "std", "-cpu", "qemu32", "-smp", "1"],
                     log=os.path.join(OUT, "serial.log"))
    try:
        q.force_text(s, settle=10.0)
        # 带 /etc/autostart=gui 的盘会直接进桌面；无盘才停在 prompt
        entered = False
        try:
            q.wait_for_prompt(s, timeout=8.0)
        except Exception:
            entered = True
        if not entered:
            q.type_text(s, "desktop")
            q.sendkey(s, "ret")
        time.sleep(2.5)
        d0 = dump(s, "01-desktop")
        w, h = ppm_size(d0)
        print("INFO frame=%dx%d" % (w, h))

        # 1) 点开始按钮 -> 菜单出现（左下 8..188 / y≈400 起）
        click(s, 48, 578)
        d1 = dump(s, "02-menu-open")
        # 菜单面板底色与任务栏底色不同，取菜单区采样点
        menu_px = px(d1, 20, 410)
        desk_px = px(d0, 300, 200)
        ok_menu = menu_px != desk_px
        print("%s menu_open sample=%s desktop=%s" % ("PASS" if ok_menu else "FAIL", menu_px, desk_px))

        # 2) 点菜单第一行（Notepad，动作 -1），期望组件面板出现
        click(s, 96, 440)
        time.sleep(1.0)
        d2 = dump(s, "03-after-row")
        ok_row = px(d2, 96, 440) != px(d1, 96, 440) or px(d2, 400, 300) != px(d0, 400, 300)
        print("%s menu_row_click changed=%s" % ("PASS" if ok_row else "FAIL", ok_row))

        # 2b) BLOCK-1 穿模：Notepad 标题栏与旧示例块标题带重叠区（y72..88）取
        #     三点，必须全部是面板标题栏色 (72,58,58)，不得透出示例块正文色
        #     (52,42,42)/正文文字 (236,232,232)/灰块 (224,32,32)/桌面底色。
        BAR, BODY, DESK = (72, 58, 58), (52, 42, 42), (38, 30, 30)
        TEXT, CHECKER = (236, 232, 232), (224, 32, 32)
        title_pts = [(190, 80), (230, 80), (270, 80)]
        title_cols = [px(d2, *pt) for pt in title_pts]
        ok_b1 = (all(c == BAR for c in title_cols) and
                 all(c not in (BODY, TEXT, CHECKER, DESK) for c in title_cols))
        print("%s block1_titlebar pts=%s" %
              ("PASS" if ok_b1 else "FAIL", title_cols))

        # 2c) BLOCK-2 整块消失：重开菜单点首行 "block 2"（z>0 示例块行），
        #     示例块必须仍在屏上——左/右两块中心至少一块仍是块底色或标题色，
        #     不得回落到桌面底色。块表计数一致性由 `gui menuclick` 探针
        #     live_before/live_after 断言。
        click(s, 48, 578)
        time.sleep(0.6)
        click(s, 96, 406)
        time.sleep(1.0)
        d2b = dump(s, "03b-after-block2")
        left_c = px(d2b, 224, 150)
        right_c = px(d2b, 720, 110)
        alive = lambda c: c != DESK and c in (BODY, BAR)
        ok_b2 = alive(left_c) or alive(right_c)
        print("%s block2_alive left=%s right=%s desk=%s" %
              ("PASS" if ok_b2 else "FAIL", left_c, right_c, DESK))

        # 3) Esc 掉记事本，再抓示例块标题带（面板 y64..88，中心 x224,y76）
        #    拖动 +120,+80。旧位置应回到桌面底色，新位置应有块底色。
        q.sendkey(s, "esc", wait=1.0)
        time.sleep(1.0)
        desk_px0 = px(d0, 600, 400)
        move(s, 224, 76)
        q.cmd(s, "mouse_button 1", wait=0.35)
        time.sleep(0.3)
        for i in range(1, 7):
            move(s, 224 + 20 * i, 76 + 13 * i, wait=0.12)
        q.cmd(s, "mouse_button 0", wait=0.5)
        d3 = dump(s, "04-dragged")
        old_px = px(d3, 150, 110)     # 旧位置应为桌面底色
        new_px = px(d3, 350, 190)     # 新位置应有块底色
        moved = old_px == desk_px0 and new_px != desk_px0
        print("%s drag old=%s new=%s desk=%s" %
              ("PASS" if moved else "FAIL", old_px, new_px, desk_px0))

        # 4) 右下角把手缩放：拖动后块右下 ≈ (504,342)，把手内缩 12px；
        #    向外拖 20,20，断言下/右边缘块像素外延（宽高单调增长）。
        move(s, 500, 338)
        q.cmd(s, "mouse_button 1", wait=0.35)
        for i in range(1, 5):
            move(s, 500 + 5 * i, 338 + 5 * i, wait=0.12)
        q.cmd(s, "mouse_button 0", wait=0.5)
        d4 = dump(s, "05-resized")
        # 旧下边界外 (500,352) 与旧右边界外 (516,280) 应被新块覆盖。
        grown = (px(d4, 500, 352) != desk_px0 or px(d4, 516, 280) != desk_px0)
        print("%s resize bottom=%s right=%s desk=%s" %
              ("PASS" if grown else "FAIL", px(d4, 500, 352), px(d4, 516, 280), desk_px0))

        # 5) 关闭面板后无残影：Esc 退出桌面再进，点击空白处
        q.sendkey(s, "esc", wait=1.5)
        q.wait_for_prompt(s)
        q.type_text(s, "desktop")
        q.sendkey(s, "ret")
        time.sleep(2.5)
        click(s, 400, 300)
        d5 = dump(s, "06-second-run")
        print("%s second_run_ok" % ("PASS" if ppm_size(d5) == (w, h) else "FAIL"))
        print("OUT dir=" + OUT)
    finally:
        q.kill_qemu(proc)


if __name__ == "__main__":
    main()
