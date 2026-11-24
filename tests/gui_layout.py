# -*- coding: utf-8 -*-
"""G-D6: 布局/几何分项探针期望字典（从 gui_probe.py 拆出）。

覆盖几何、边界、命中、缩放、在场等与块树布局相关的子命令；
像素刷写类用例见 gui_paint.py。gui_probe.py 仍为兼容入口，按子命令合并各分项字典。
"""

EXPECT = {
    "geom":     {"latest": {}, "flag": None},
    "bounds":   {"latest": {"result": "1"}, "flag": None},
    "hit":      {"latest": {"result": "1"}, "flag": None},
    "resize":   {"latest": {"result": "1"}, "flag": None},
    "presence": {"latest": {"result": "1"}, "flag": None},
}
