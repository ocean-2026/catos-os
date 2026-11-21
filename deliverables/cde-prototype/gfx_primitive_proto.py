#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gfx_primitive_proto.py -- 宿主原型：演示 P1 新增 CE ABI 原语如何驱动一个
纯用户态窗口 + 事件循环（不碰内核，先给用户看实际效果）。

背景：参考 CDE 的内核全功能方案里，窗口/控件/事件泵焊死在内核。我们的
方案把它们上移到 desk.ce，内核只提供通用原语。经盘点，现有 CE ABI 缺
两类关键原语：
  * 鼠标状态 (mouse_x / mouse_y / mouse_buttons) -- 完全缺失
  * 像素文本绘制 (gfx_text) -- 现只有 gfx_rect/gfx_blit，无文字
P1 因此追加 6 个通用原语槽 (66..71)：
  66 mouse_x()        67 mouse_y()       68 mouse_buttons()
  69 gfx_text(x,y,fg,s)  70 key_peek()   71 key_mods()

本原型在宿主用 80x24 字符网格模拟 800x600 RGB 帧缓冲，运行一个真实
的事件循环：鼠标移动 -> 命中测试 -> 拖标题栏 -> 点击按钮 -> 焦点切换。
运行： python gfx_primitive_proto.py
"""
import sys
import time

W, H = 80, 24


# ---- 模拟内核原语 (对应 P1 新增的 CE ABI 槽) ---------------------------
class VTable:
    """模拟注入给 CE 程序的 catos_api_t 新增原语。"""

    def __init__(self):
        self.fb = [[" "] * W for _ in range(H)]
        self.mouse = [40, 12, 0]      # x, y, buttons bitmask
        self.keyq = []                # pending scancodes
        self.mods = 0

    # slot 66/67/68
    def mouse_x(self): return self.mouse[0]
    def mouse_y(self): return self.mouse[1]
    def mouse_buttons(self): return self.mouse[2]

    # slot 69 -- 把字符串画到字符网格 (对应 gfx_text)
    def gfx_text(self, x, y, fg, s):
        for i, ch in enumerate(s):
            if 0 <= x + i < W and 0 <= y < H:
                self.fb[y][x + i] = ch

    def gfx_rect(self, x, y, w, h, ch):
        for r in range(y, y + h):
            for c in range(x, x + w):
                if 0 <= r < H and 0 <= c < W:
                    self.fb[r][c] = ch

    # slot 70/71
    def key_peek(self): return self.keyq[0] if self.keyq else -1
    def key_mods(self): return self.mods

    def clear(self):
        self.fb = [[" "] * W for _ in range(H)]

    def present(self):
        print("+" + "-" * W + "+")
        for row in self.fb:
            print("|" + "".join(row) + "|")
        print("+" + "-" * W + "+")


# ---- 用户态桌面 (desk.ce 的逻辑，全部跑在 CE 侧) -----------------------
class Window:
    def __init__(self, wid, title, x, y, w, h, body):
        self.wid, self.title = wid, title
        self.x, self.y, self.w, self.h = x, y, w, h
        self.body = body
        self.body_lines = []
        for raw in body.split("\n"):
            for i in range(0, len(raw), w - 4):
                self.body_lines.append(raw[i:i + w - 4])

    def hit_title(self, mx, my):
        return self.x <= mx < self.x + self.w and my == self.y

    def hit_body(self, mx, my):
        return (self.x <= mx < self.x + self.w and
                self.y < my < self.y + self.h)


def draw_window(vt, win, focused):
    border = "=" if focused else "-"
    vt.gfx_rect(win.x, win.y, win.w, win.h, " ")
    # title bar
    vt.gfx_text(win.x, win.y, 15, "+" + border * (win.w - 2) + "+")
    vt.gfx_text(win.x + 1, win.y, 15, "[" + win.title + "]")
    # body
    for i, line in enumerate(win.body_lines[:win.h - 2]):
        vt.gfx_text(win.x + 2, win.y + 1 + i, 7, line)
    # bottom
    vt.gfx_text(win.x, win.y + win.h - 1, 15, "+" + border * (win.w - 2) + "+")


def compose(vt, wins, focused_wid):
    vt.clear()
    for win in wins:
        draw_window(vt, win, win.wid == focused_wid)
    # taskbar (用户态画的)
    vt.gfx_rect(0, H - 1, W, 1, "-")
    labels = " ".join("[%s]" % w.title for w in wins)
    vt.gfx_text(1, H - 1, 15, "[Start] " + labels)
    vt.gfx_text(W - 16, H - 1, 15, "focus=%d" % focused_wid)


def event_loop(vt, wins, script):
    focused = wins[0].wid
    drag = None
    print("=== 事件循环：鼠标移动/拖拽/点击 + 焦点切换 ===")
    for step, (mx, my, btn, note) in enumerate(script):
        vt.mouse = [mx, my, btn]
        # --- 用新增原语读输入 ---
        x, y, b = vt.mouse_x(), vt.mouse_y(), vt.mouse_buttons()
        # --- 用户态命中测试 (窗口管理逻辑在 CE 侧) ---
        if b & 1:                          # 左键按下
            for w in wins:
                if w.hit_title(x, y):
                    focused = w.wid
                    drag = (w, x - w.x, y - w.y)
                    break
            if drag is None:
                for w in wins:
                    if w.hit_body(x, y):
                        focused = w.wid
                        break
        else:
            drag = None
        if drag:                           # 拖标题栏移动窗口 (CE 侧逻辑)
            w, dx, dy = drag
            w.x = max(0, min(W - w.w, x - dx))
            w.y = max(0, min(H - 2, y - dy))
        compose(vt, wins, focused)
        print("\n[%02d] mouse=(%d,%d) btn=%d  %s" % (step, x, y, b, note))
        vt.present()
        time.sleep(0.15)


def main():
    vt = VTable()
    wins = [
        Window(1, "Term", 4, 3, 28, 9, "catos:/# ls -l\nhello.ce  desk.ce\nbc.ce     pkgs.txt"),
        Window(2, "Files", 38, 6, 30, 10, "/bin/\n  desk.ce\n  hello.ce\n  gfxdemo.ce\n/pkg/\n  desktop.catpkg"),
    ]
    # 演示脚本：(mouse_x, mouse_y, buttons, note)
    script = [
        (40, 5, 0, "初始：Term 聚焦，鼠标空闲"),
        (45, 6, 1, "点 Files 标题栏 -> 焦点切换到 Files"),
        (47, 8, 1, "拖 Files 标题栏 -> 窗口右移下移"),
        (47, 8, 0, "松开左键 -> 停止拖拽"),
        (10, 4, 0, "鼠标移回 Term -> 焦点保持 Files"),
        (10, 4, 1, "点 Term 标题栏 -> 焦点切回 Term"),
    ]
    event_loop(vt, wins, script)
    print("\n原语调用统计（证明窗口语义全在 CE 侧，内核只提供原语）：")
    print("  mouse_x/y/buttons: 事件循环每轮读取")
    print("  gfx_rect/gfx_text: 合成窗口与任务栏")
    print("  key_peek/key_mods: 预留给键盘快捷方式 (Alt+Tab 等)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
