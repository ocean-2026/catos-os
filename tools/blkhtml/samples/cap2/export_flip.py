#!/usr/bin/env python3
"""导出 popover 四向翻转对照图（可复现，离线 preview）。

用法：python3 export_flip.py [out_dir]
对 popover-flip.blk.json 逐浮层过滤出「锚按钮 + 单个浮层」的子树，
按 pop_layout 定位后各出一张 PNG，方向由 data-prefer 决定。
"""
import copy
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", ".."))
import preview  # noqa: E402

SRC = os.path.join(HERE, "popover-flip.blk.json")
DIR_NAME = {1: "up", 2: "down", 4: "left", 8: "right"}


def filter_tree(tree, keep_ids):
    root = copy.deepcopy(tree["root"])

    def walk(n):
        n["children"] = [c for c in n.get("children", [])
                         if c["id"] in keep_ids]
        for c in n["children"]:
            walk(c)
    walk(root)
    out = dict(tree)
    out["root"] = root
    return out


def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "out")
    os.makedirs(out_dir, exist_ok=True)
    tree = json.load(open(SRC, encoding="utf-8"))
    by_id = {n["id"]: n for n in preview.bc.iter_nodes(tree["root"])}
    pops = [n for n in preview.bc.iter_nodes(tree["root"])
            if n["kind"] == "popover"]
    for pop in pops:
        prefer = pop.get("pop_prefer", 0)
        name = DIR_NAME.get(prefer, "dir%d" % prefer)
        keep = {pop["anchor_id"], pop["id"]}
        for c in pop.get("children", []):
            keep.add(c["id"])
        sub = filter_tree(tree, keep)
        buf = preview.render_png(sub, "app", "dark")
        path = os.path.join(out_dir, "flip-%s.png" % name)
        preview.write_png(path, buf)
        print("wrote", path)


if __name__ == "__main__":
    main()
