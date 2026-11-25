#!/usr/bin/env python3
"""validate -- 按 schema :: validation_rules 逐条断言 .blk.json / 二进制记录。

规则：C1 G1 G2 B1 B2 R1 S1 S2 D1 T1 L1 O1 P1。
任一 FAIL → 退出 1；否则 0。

用法：
    python3 validate.py page.blk.json [--json] [--ce]
    python3 validate.py --bin --recs recs.bin --strings strings.bin [--json]

纯 Python 3 标准库，零第三方依赖。
"""

import argparse
import json
import os
import struct
import sys

import blkcommon as bc

# CE 槽 72 可提交的 kind 区间（decl.c validate(): COLUMN..CANVAS）。
# button(12)/input(13)/term(14) 一律拒收（E_DECL_BADREC = -39）。
CE_KIND_OK = {"column", "row", "stack", "text", "rect", "image",
              "surface", "spacer", "divider", "canvas"}


class Report:
    def __init__(self):
        self.results = []

    def record(self, rule, obj, ok, detail="", level=None):
        # level: 缺省按 ok 推导 PASS/FAIL；越界可传 "WARN"（不计入 failed）。
        if level is None:
            level = "PASS" if ok else "FAIL"
        self.results.append({"rule": rule, "object": obj,
                             "status": level, "detail": detail})

    @property
    def failed(self):
        return any(r["status"] == "FAIL" for r in self.results)

    @property
    def warned(self):
        return [r for r in self.results if r["status"] == "WARN"]

    def text_lines(self):
        return ["%s %s %s %s" % (r["status"], r["rule"], r["object"],
                                 r["detail"]) for r in self.results]


# ---------------------------------------------------------------------------
# JSON 树校验
# ---------------------------------------------------------------------------


def validate_tree(tree, rep, ce_projection=False):
    root = tree["root"]
    nodes = list(bc.iter_nodes(root))
    by_id = {n["id"]: n for n in nodes}

    # ---- C1：id 唯一、parent 存在、无环、配额 ----
    ids = [n["id"] for n in nodes]
    if root["id"] != 0:
        rep.record("C1", "root", False, "root id must be 0, got %d" % root["id"])
    elif ce_projection:
        # CE 投影：根记录丢弃，子块 parent=0 挂到内核 decl_ensure_root 自建的
        # 应用根；dektest.c 即此约定。根 id=0 不判 FAIL。
        rep.record("C1", "root", True,
                   "根 id=0 → CE 投影丢弃，子块 parent=0 挂应用根")
    else:
        rep.record("C1", "root", True, "root id=0")
    seen = set()
    for n in nodes:
        if n["id"] in seen:
            rep.record("C1", "id=%s" % n["id"], False, "duplicate id")
        seen.add(n["id"])
        if n["id"] != 0 and n["id"] < 1:
            rep.record("C1", "id=%s" % n["id"], False, "non-root id must be >=1")
    rep.record("C1", "count", len(nodes) <= bc.QUOTAS["max_blocks"],
               "%d blocks (quota %d)" % (len(nodes), bc.QUOTAS["max_blocks"]))

    pmap = bc.parent_map(root)
    for n in nodes:
        pid = pmap[n["id"]]
        if pid != 0 and pid not in by_id:
            rep.record("C1", "id=%s" % n["id"], False,
                       "parent %d missing" % pid)

    # 无环 + 深度
    def depth(nid, stack):
        if nid in stack:
            rep.record("C1", "id=%s" % nid, False, "cycle detected")
            return 0
        stack.add(nid)
        pid = pmap[nid]
        d = 1 if pid == 0 else depth(pid, stack) + 1
        stack.discard(nid)
        return d

    max_depth = 0
    for n in nodes:
        d = depth(n["id"], set())
        max_depth = max(max_depth, d)
    rep.record("C1", "depth", max_depth <= bc.QUOTAS["max_depth"],
               "max depth %d (quota %d)" % (max_depth, bc.QUOTAS["max_depth"]))

    # ---- G1：rect 8 的倍数 ----
    for n in nodes:
        r = n["rect"]
        bad = [k for k in ("x", "y", "w", "h") if r[k] % 8 != 0]
        rep.record("G1", "id=%s" % n["id"], not bad,
                   "rect=%s" % r if bad else "")

    # ---- G2：spacing 属于允许档 ----
    allowed_pad = set(bc.schema()["allowed_spacing"]["padding"])
    allowed_gap = set(bc.schema()["allowed_spacing"]["gap"])
    allowed_mg = set(bc.schema()["allowed_spacing"]["margin"])
    for n in nodes:
        obj = "id=%s" % n["id"]
        pad = n.get("padding", 0)
        pad_vals = pad.values() if isinstance(pad, dict) else [pad]
        if any(v not in allowed_pad for v in pad_vals):
            rep.record("G2", obj, False, "padding %s" % pad)
        mg = n.get("margin", 0)
        mg_vals = mg.values() if isinstance(mg, dict) else [mg]
        if any(v not in allowed_mg for v in mg_vals):
            rep.record("G2", obj, False, "margin %s" % mg)
        gap = (n.get("layout") or {}).get("gap")
        if gap is not None and gap not in allowed_gap:
            rep.record("G2", obj, False, "gap %s" % gap)
    rep.record("G2", "spacing", True, "")

    # ---- B1 / B2：边界 ----
    def inside(child, parent_rect):
        return (child["x"] >= parent_rect["x"] and
                child["y"] >= parent_rect["y"] and
                child["x"] + child["w"] <= parent_rect["x"] + parent_rect["w"] and
                child["y"] + child["h"] <= parent_rect["y"] + parent_rect["h"])

    b1_fail = b1_warn = b2_fail = False
    for n in nodes:
        ovf = n.get("overflow", "clip")
        for c in n.get("children") or []:
            obj = "id=%s→%s" % (n["id"], c["id"])
            if not inside(c["rect"], n["rect"]):
                rep.record("B2", obj, False,
                           "child outside parent outer rect %s" % c["rect"])
                b2_fail = True
            if not inside(c["rect"], bc.content_rect(n)):
                if ovf == "clip":
                    rep.record("B1", obj, False,
                               "clip overflow: child outside content rect")
                    b1_fail = True
                else:
                    # visible/scroll 允许越界，但必须显式声明（schema B1）。
                    rep.record("B1", obj, True,
                               "overflow=%s 越界（已显式声明）" % ovf,
                               level="WARN")
                    b1_warn = True
    if not b2_fail:
        rep.record("B2", "outer bounds", True, "")
    if not b1_fail:
        rep.record("B1", "clip bounds", True,
                   "%d warning(s)" % b1_warn if b1_warn else "")

    # ---- R1 / S1 / S2 / D1：贴边规则 ----
    for n in nodes:
        for c in n.get("children") or []:
            sides = bc.touch_sides(c, n)
            obj = "id=%s→%s" % (n["id"], c["id"])
            if not sides:
                continue
            rad = c.get("radius") or {}
            for s in sides:
                for corner in bc.SIDE_CORNERS[s]:
                    if rad.get(corner, 0) != 0:
                        rep.record("R1", obj, False,
                                   "side %s corner %s radius=%d" %
                                   (s, corner, rad[corner]))
            stroke = c.get("stroke") or {}
            for s in sides:
                if s in (stroke.get("sides") or []):
                    rep.record("S1", obj, False, "stroke on flush side %s" % s)
            shadow = c.get("shadow") or {}
            for s in sides:
                if s in (shadow.get("sides") or []):
                    rep.record("S2", obj, False, "shadow on flush side %s" % s)
            # D1：父子不得在同侧都画 >=2px
            p_stroke = n.get("stroke") or {}
            c_stroke = stroke
            for s in sides:
                if (p_stroke.get("w", 0) >= 2 and
                        c_stroke.get("w", 0) >= 2 and
                        s in (p_stroke.get("sides") or ["L", "R", "T", "B"]) and
                        s in (c_stroke.get("sides") or ["L", "R", "T", "B"])):
                    rep.record("D1", obj, False,
                               "double %dpx border on side %s" %
                               (p_stroke["w"], s))
    rep.record("R1", "flush corners", True, "")
    rep.record("S1", "flush stroke", True, "")
    rep.record("S2", "flush shadow", True, "")
    rep.record("D1", "shared edge", True, "")

    # ---- T1：文本度量 ----
    for n in nodes:
        if not n.get("text"):
            continue
        scale = (n.get("font") or {}).get("scale", 1)
        r = n["rect"]
        tw = bc.text_width(n["text"], scale)
        content_w = r["w"] - 8
        fits = tw <= content_w
        truncated = n.get("text_overflow", "ellipsis") == "ellipsis"
        h_ok = 16 * scale <= r["h"]
        rep.record("T1", "id=%s" % n["id"], fits or truncated,
                   "text_w=%d content_w=%d h=%d" % (tw, content_w, r["h"]))
        if not h_ok:
            rep.record("T1", "id=%s" % n["id"], False, "text h overflow")

    # ---- L1：layer 合法；CE 投影只允许 work ----
    for n in nodes:
        layer = n.get("layer", "work")
        legal = layer in bc.LAYER_FROM_NAME
        rep.record("L1", "id=%s" % n["id"], legal, "layer=%s" % layer)
        if ce_projection and layer != "work":
            rep.record("L1", "id=%s" % n["id"], False,
                       "CE projection accepts work only, got %s" % layer)
        if ce_projection and n["id"] != 0 and n["kind"] not in CE_KIND_OK:
            rep.record("L1", "id=%s" % n["id"], False,
                       "CE projection rejects kind=%s: decl.c validate() "
                       "allows COLUMN..CANVAS only" % n["kind"])

    # ---- O1：scroll 偏移不超出溢出范围 ----
    for n in nodes:
        if n.get("overflow") != "scroll":
            continue
        scroll = n.get("scroll") or {}
        r = n["rect"]
        ox = scroll.get("x", 0)
        oy = scroll.get("y", 0)
        ok = 0 <= ox <= r["w"] and 0 <= oy <= r["h"]
        rep.record("O1", "id=%s" % n["id"], ok,
                   "scroll=%s rect=%s" % (scroll, r))
    rep.record("O1", "scroll range", True, "")


# ---------------------------------------------------------------------------
# 二进制 P1 复核
# ---------------------------------------------------------------------------


def validate_bin(rep, recs_path, strings_path):
    with open(recs_path, "rb") as f:
        recs = f.read()
    with open(strings_path, "rb") as f:
        pool = f.read()

    n = len(recs) // bc.REC_BYTES
    rep.record("P1", "recs length", len(recs) == n * bc.REC_BYTES,
               "%d bytes = %d * %d" % (len(recs), n, bc.REC_BYTES))

    if len(recs) % bc.REC_BYTES != 0:
        rep.record("P1", "recs", False, "not a multiple of %d" % bc.REC_BYTES)
        return

    for i in range(n):
        raw = recs[i * bc.REC_BYTES:(i + 1) * bc.REC_BYTES]
        (rid, parent, kind, pad0, layer, req_w, req_h,
         text_off, pix_off, pix_w, pix_h, res) = struct.unpack(
            bc.REC_FMT, raw)
        obj = "rec[%d] id=%d" % (i, rid)
        if pad0 != 0:
            rep.record("P1", obj, False, "pad0=%d must be 0" % pad0)
        if pix_off != -1:
            rep.record("P1", obj, False, "pix_off=%d must be -1" % pix_off)
        if text_off != -1 and not (0 <= text_off < len(pool)):
            rep.record("P1", obj, False,
                       "text_off=%d out of pool len=%d" % (text_off, len(pool)))
        if res != -1 and res < 0:
            rep.record("P1", obj, False, "res=%d" % res)
        if text_off != -1:
            end = pool.find(b"\x00", text_off)
            if end == -1:
                rep.record("P1", obj, False, "no NUL terminator")
    rep.record("P1", "pool offsets", True, "pool=%d bytes" % len(pool))


# ---------------------------------------------------------------------------


def main(argv=None):
    ap = argparse.ArgumentParser(description="blkhtml 规则校验")
    ap.add_argument("blkjson", nargs="?", help="输入 .blk.json")
    ap.add_argument("--json", action="store_true", help="机读 JSON 输出")
    ap.add_argument("--ce", action="store_true",
                    help="CE 投影口径：layer 只允许 work")
    ap.add_argument("--bin", action="store_true",
                    help="对 recs.bin/strings.bin 做 P1 复核")
    ap.add_argument("--recs", help="recs.bin 路径（--bin）")
    ap.add_argument("--strings", help="strings.bin 路径（--bin）")
    args = ap.parse_args(argv)

    rep = Report()

    if args.bin:
        if not args.recs or not args.strings:
            ap.error("--bin 需要 --recs 与 --strings")
        validate_bin(rep, args.recs, args.strings)
    else:
        if not args.blkjson:
            ap.error("需要 .blk.json 或 --bin")
        with open(args.blkjson, "r", encoding="utf-8") as f:
            tree = json.load(f)
        validate_tree(tree, rep, ce_projection=args.ce)

    if args.json:
        print(json.dumps({"failed": rep.failed, "results": rep.results},
                         ensure_ascii=False, indent=2))
    else:
        for line in rep.text_lines():
            print(line)
        print("FAIL=%d" % sum(r["status"] == "FAIL" for r in rep.results),
              file=sys.stderr)

    return 1 if rep.failed else 0


if __name__ == "__main__":
    sys.exit(main())
