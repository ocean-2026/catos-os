#!/usr/bin/env python3
"""样例后补脚本（不是工具链的一部分；不改 tools/blkhtml/*.py）。

HTML 受控子集表达不了、但 schema 声明层支持的字段，在这里机械回填：
  - notepad 窗口：resize.dir = [R, B]（标题带 drag=title 已由 data-blk-drag 带入）
  - fullbleed 横幅：bleed = true
  - startmenu：html2blk 的 free 分支不向子块下传定位（见报告 bug 清单 B2），
    按锚定后的菜单视口 (536,8,256,480) 把菜单项/分隔线/用户行矩形补齐到
    8pt 量化坐标；锚点/px/layer/overflow 仍以 html2blk 产出为准。

用法：python3 _patch_decl.py
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))


def load(name):
    p = os.path.join(HERE, name + ".blk.json")
    with open(p, "r", encoding="utf-8") as f:
        return json.load(f), p


def save(tree, p):
    with open(p, "w", encoding="utf-8") as f:
        json.dump(tree, f, ensure_ascii=False, indent=2)


def patch_notepad():
    tree, p = load("notepad")
    win = tree["root"]["children"][0]
    win["resize"] = {"dir": ["R", "B"], "edge": 6, "min_w": 24,
                     "min_h": 24, "max_w": 0, "max_h": 0, "step": 0}
    save(tree, p)


def patch_fullbleed():
    tree, p = load("fullbleed")
    banner = tree["root"]["children"][0]
    banner["bleed"] = True
    save(tree, p)


def patch_startmenu():
    tree, p = load("startmenu")
    outer = tree["root"]["children"][0]
    menu = outer["children"][0]
    mx, my, mw, mh = menu["rect"]["x"], menu["rect"]["y"], \
        menu["rect"]["w"], menu["rect"]["h"]
    pad = 8
    ix = mx + pad
    iw = mw - pad * 2
    items = [c for c in menu["children"] if c["kind"] == "button"]
    divider = [c for c in menu["children"] if c["kind"] == "divider"]
    user = items[-1]
    item_btns = items[:-1]
    pitch = 24
    for i, btn in enumerate(item_btns):
        btn["rect"] = {"x": ix, "y": my + pad + pitch * i,
                       "w": iw, "h": 24}
    if divider:
        divider[0]["rect"] = {"x": ix, "y": my + pad + pitch * len(item_btns) + 8,
                              "w": iw, "h": 8}
    user["rect"] = {"x": ix, "y": my + mh - pad - 24, "w": iw, "h": 24}
    save(tree, p)


if __name__ == "__main__":
    patch_notepad()
    patch_fullbleed()
    patch_startmenu()
    print("patched notepad / fullbleed / startmenu")
