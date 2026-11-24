# -*- coding: utf-8 -*-
"""G-D6: 幂等性分项探针期望字典（从 gui_probe.py 拆出）。

覆盖重复提交/重建的树哈希一致性与复位后状态。
"""

EXPECT = {
    "idem":  {"latest": {"match": "1"}, "flag": "GUI:idempotent=OK"},
    "reset": {"latest": {"result": "1"}, "flag": None},
}
