# -*- coding: utf-8 -*-
"""G-D6: 刷写/画面分项探针期望字典（从 gui_probe.py 拆出）。

覆盖脏矩形、差分、空闲、主题、菜单、主页、记事本、画布、文件管理、
图片、浏览器、cvchan、sak、手势、速率等与帧合成/像素呈现相关的子命令。
"""

EXPECT = {
    "dirty":   {"latest": {"result": "1"}, "flag": None},
    "diff":    {"latest": {"result": "1"}, "flag": None},
    "idle":    {"latest": {"result": "1"}, "flag": None},
    "theme":   {"latest": {"result": "1"}, "flag": None},
    "menu":    {"latest": {"result": "1"}, "flag": None},
    "home":    {"latest": {"result": "1"}, "flag": None},
    "note":    {"latest": {"result": "1"}, "flag": None},
    "cv":      {"latest": {"result": "1"}, "flag": None},
    "fm":      {"latest": {"result": "1"}, "flag": None},
    "img":     {"latest": {"ok": "1"}, "flag": None},
    "br":      {"latest": {"result": "1"}, "flag": None},
    "cvchan":  {"latest": {"result": "1"}, "flag": None},
    "sak":     {"latest": {"result": "1"}, "flag": None},
    "gesture": {"latest": {"result": "1"}, "flag": None},
    "rate":    {"latest": {"result": "1"}, "flag": None},
}
