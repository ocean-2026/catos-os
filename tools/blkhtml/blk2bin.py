#!/usr/bin/env python3
"""blk2bin -- .blk.json → recs.bin + strings.bin（+ --emit-c 内置页 C 片段）。

用法：
    python3 blk2bin.py page.blk.json [--out-prefix P] [--emit-c page.c]
                       [--stats] [--patch-json]

- recs.bin：40*n 字节，pad0=0；text_off 为 strings 池内偏移或 -1。
- strings.bin：按首次出现顺序拼接，同文本去重，NUL 结尾。
- 偏移在写入后回读校验（不是估算）。
- --emit-c：产出内核内置页可用的 C 片段（static blk_spec / blk_desc 数组
  + blkgen_commit()）；超集字段按 blk_spec 逐字段赋值。

纯 Python 3 标准库，零第三方依赖。
"""

import argparse
import json
import os
import re
import struct
import sys

import blkcommon as bc

QUOTA = bc.QUOTAS


# ---------------------------------------------------------------------------
# 树收集与串池
# ---------------------------------------------------------------------------


def load_tree(path):
    with open(path, "r", encoding="utf-8") as f:
        tree = json.load(f)
    return tree["root"], tree


def collect_nodes(root):
    """先序遍历，保证父记录先于子记录（与 decl_commit 父链要求一致）。"""
    out = []

    def walk(n):
        out.append(n)
        for c in n.get("children") or []:
            walk(c)

    walk(root)
    return out


def build_pool(nodes):
    """返回 (pool_bytes, {node_id: text_off})。同文本去重；首次出现定序。"""
    pool = bytearray()
    offsets = {}
    seen = {}
    for n in nodes:
        text = n.get("text")
        if not text:
            offsets[n["id"]] = -1
            continue
        raw = text.encode("utf-8")
        if text in seen:
            offsets[n["id"]] = seen[text]
            continue
        off = len(pool)
        seen[text] = off
        offsets[n["id"]] = off
        pool += raw
        pool += b"\x00"
    return bytes(pool), offsets


def serialize_rec(node, text_off):
    ce = node.get("ce") or {}
    kind = ce.get("kind", bc.KIND_FROM_NAME[node["kind"]])
    layer = ce.get("layer", bc.LAYER_FROM_NAME.get(node.get("layer", "work"), 200))
    req = node.get("req", {})
    req_w = ce.get("req_w", req.get("w", -1))
    req_h = ce.get("req_h", req.get("h", -1))
    pix = node.get("pix") or {}
    pix_w = ce.get("pix_w", pix.get("w", 0))
    pix_h = ce.get("pix_h", pix.get("h", 0))
    res = ce.get("res", pix.get("res", -1))
    pix_off = ce.get("pix_off", -1)
    return struct.pack(bc.REC_FMT,
                       int(ce.get("id", node["id"])),
                       int(ce.get("parent", 0)),
                       int(kind), 0, int(layer),
                       int(req_w), int(req_h),
                       int(text_off),
                       int(pix_off),
                       int(pix_w), int(pix_h), int(res))


def verify_pool(pool, offsets, nodes):
    """回读校验：每个 text_off 指向 NUL 结尾且内容与原文一致。"""
    by_id = {n["id"]: n for n in nodes}
    for nid, off in offsets.items():
        if off == -1:
            continue
        if off < 0 or off >= len(pool):
            raise ValueError("text_off %d out of pool (len=%d)" % (off, len(pool)))
        end = pool.find(b"\x00", off)
        raw = pool[off:end]
        if raw.decode("utf-8") != by_id[nid].get("text", ""):
            raise ValueError("pool round-trip mismatch at off %d" % off)


# ---------------------------------------------------------------------------
# --emit-c 片段
# ---------------------------------------------------------------------------

BLK_ENUM = {v: "BLK_" + k.upper() for k, v in bc.KIND_FROM_NAME.items()}
ALN_MAIN = {"start": "ALN_START", "center": "ALN_CENTER", "end": "ALN_END",
            "space_between": "ALN_SPACE_BETWEEN"}
ALN_CROSS = {"start": "XALN_START", "center": "XALN_CENTER", "end": "XALN_END",
             "stretch": "XALN_STRETCH"}
OVF_ENUM = {"clip": "OVF_CLIP", "visible": "OVF_VISIBLE", "scroll": "OVF_SCROLL"}
DRAG_ENUM = {"none": "DRAG_NONE", "title": "DRAG_TITLE", "full": "DRAG_FULL"}
BOUND_ENUM = {"free": "BOUND_FREE", "parent": "BOUND_PARENT",
               "screen": "BOUND_SCREEN"}
RESIZE_DIR = {"L": "RESIZE_L", "R": "RESIZE_R", "T": "RESIZE_T",
              "B": "RESIZE_B"}
ANCHOR_ENUM = bc.ANCHOR_BITS

# P1：HTML 角色名 → enum gui_role（tokens.h），角色直给、不按 layer 反推。
# ROLE_BG 与 ROLE_PANEL 是两个不同角色，panel 必须映射到 ROLE_PANEL。
ROLE_ENUM = {"fg": "ROLE_FG", "bg": "ROLE_BG", "panel": "ROLE_PANEL",
             "bar": "ROLE_BAR", "border": "ROLE_BORDER",
             "line": "ROLE_LINE", "accent": "ROLE_ACCENT"}


def color_expr(raw):
    """返回 (C 颜色字面量, role 枚举)。

    - 角色名：颜色写 INHERIT_COLOR，role_* 置对应枚举；
    - `#RRGGBB` / `RRGGBB`：固化 RGB 进 fg/bg/accent，role=ROLE_NONE；
    - 空：继承，ROLE_NONE。
    """
    if not raw:
        return "INHERIT_COLOR", "ROLE_NONE"
    if raw in bc.ROLE_NAMES:
        return "INHERIT_COLOR", ROLE_ENUM[raw]
    h = raw.lstrip("#")
    if re.fullmatch(r"[0-9a-fA-F]{6}", h):
        return "0x%su" % h.upper(), "ROLE_NONE"
    return "INHERIT_COLOR", "ROLE_NONE"


def emit_kind_name(n):
    """B7：emit-c 的 kind 优先按 flow 映射，不信 JSON 里可能漂移的 kind。"""
    flow = (n.get("layout") or {}).get("flow")
    if flow == "row":
        return "row"
    if flow in ("column", "stack"):
        return "column"
    return n["kind"]


def c_str(s):
    return json.dumps(s, ensure_ascii=False)


def anchor_mask(anchors):
    if not anchors:
        return "0"
    parts = ["ANC_" + a for a in anchors if a in ANCHOR_ENUM]
    return " | ".join(parts) if parts else "0"


def resize_dir_mask(dir_list):
    if not dir_list:
        return "0"
    return " | ".join(RESIZE_DIR[d] for d in dir_list if d in RESIZE_DIR)


def emit_c(tree, nodes, pool):
    lines = []
    lines.append("/* auto-generated by blk2bin --emit-c; do not edit by hand.")
    lines.append(" * 目标：内核内置页（blk_spec 全字段），不经 CE 槽 72。")
    lines.append(" * 注意：槽 72 的 ce_blk_rec_t 不携带几何/样式；全保真只能走本片段。")
    lines.append(" */")
    lines.append("#include <gui/gui.h>")
    lines.append("")

    # 规格数组
    for n in nodes:
        nid = n["id"]
        spec_name = "spec_%d" % nid
        pad = n.get("padding", 0)
        if isinstance(pad, int):
            pt = pr = pb = pl = pad
        else:
            pt = pad.get("t", 0); pr = pad.get("r", 0)
            pb = pad.get("b", 0); pl = pad.get("l", 0)
        mg = n.get("margin", 0)
        if isinstance(mg, int):
            mt = mr = mb = ml = mg
        else:
            mt = mg.get("t", 0); mr = mg.get("r", 0)
            mb = mg.get("b", 0); ml = mg.get("l", 0)
        layout = n.get("layout") or {}
        req = n.get("req", {})
        anchor = anchor_mask(n.get("anchor"))
        pct = n.get("pct") or {}
        px = n.get("px") or {}
        mm = n.get("min_max") or {}
        resize = n.get("resize") or {}
        scroll = n.get("scroll") or {}
        font = n.get("font") or {}
        stroke = n.get("stroke") or {}

        fields = []
        fields.append(".req_w = %d" % (req.get("w", -1)))
        fields.append(".req_h = %d" % (req.get("h", -1)))
        fields.append(".pad_t = %d, .pad_r = %d, .pad_b = %d, .pad_l = %d"
                      % (pt, pr, pb, pl))
        fields.append(".gap = %d" % layout.get("gap", 0))
        am = ALN_MAIN.get(layout.get("align_main", "start"), "ALN_START")
        ac = ALN_CROSS.get(layout.get("align_cross", "start"), "XALN_START")
        fields.append(".align_main = %s, .align_cross = %s" % (am, ac))
        layer = bc.LAYER_FROM_NAME.get(n.get("layer", "work"), 200)
        fields.append(".layer = %d" % layer)
        fields.append(".z = %d" % n.get("z", 0))
        fields.append(".visible = %d, .focusable = %d, .inert = %d"
                      % (1 if n.get("visible", True) else 0,
                         1 if n.get("focusable", False) else 0,
                         1 if n.get("inert", False) else 0))
        # 颜色：角色名走 role_* 枚举；字面 RGB 才固化进 fg/bg/accent（B6）。
        fill = n.get("fill"); fg = n.get("fg"); acc = n.get("accent")
        fg_c, fg_r = color_expr(fg)
        bg_c, bg_r = color_expr(fill)
        ac_c, ac_r = color_expr(acc)
        fields.append(".fg = %s, .bg = %s, .accent = %s"
                      % (fg_c, bg_c, ac_c))
        fields.append(".role_fg = %s, .role_bg = %s, .role_accent = %s"
                      % (fg_r, bg_r, ac_r))
        fields.append(".scale = %d, .border_w = %d"
                      % (font.get("scale", 1), stroke.get("w", 0)))
        fields.append(".overflow = %s"
                      % OVF_ENUM.get(n.get("overflow", "clip"), "OVF_CLIP"))
        fields.append(".anchor = %s" % anchor)
        fields.append(".pct_x = %d, .pct_y = %d, .pct_w = %d, .pct_h = %d"
                      % (pct.get("x", 0), pct.get("y", 0),
                         pct.get("w", 0), pct.get("h", 0)))
        fields.append(".px_x = %d, .px_y = %d" % (px.get("x", 0), px.get("y", 0)))
        fields.append(".min_w = %d, .min_h = %d, .max_w = %d, .max_h = %d"
                      % (mm.get("min_w", 0), mm.get("min_h", 0),
                         mm.get("max_w", 0), mm.get("max_h", 0)))
        fields.append(".m_t = %d, .m_r = %d, .m_b = %d, .m_l = %d"
                      % (mt, mr, mb, ml))
        fields.append(".drag_mode = %s, .drag_bounds = %s"
                      % (DRAG_ENUM.get(n.get("drag", "none"), "DRAG_NONE"),
                         BOUND_ENUM.get(n.get("drag_bounds", "free"),
                                        "BOUND_FREE")))
        fields.append(".resize_dir = %s, .resize_edge = %d"
                      % (resize_dir_mask(resize.get("dir", [])),
                         resize.get("edge", 6)))
        fields.append(".resize_min_w = %d, .resize_min_h = %d"
                      % (resize.get("min_w", 24), resize.get("min_h", 24)))
        fields.append(".resize_max_w = %d, .resize_max_h = %d, .resize_step = %d"
                      % (resize.get("max_w", 0), resize.get("max_h", 0),
                         resize.get("step", 0)))
        fields.append(".scroll_x = %d, .scroll_y = %d"
                      % (scroll.get("x", 0), scroll.get("y", 0)))

        lines.append("static blk_spec %s = {" % spec_name)
        for f in fields:
            lines.append("    %s," % f)
        lines.append("};")
        lines.append("")

    # 描述数组：desc 直接以内联聚合初始化进 descs[]（静态初始化器不能引用
    # 另一个静态对象；分开声明 static blk_desc 会被 gcc 判为非常量）。
    desc_inits = []
    for n in nodes:
        nid = n["id"]
        kind_id = bc.KIND_FROM_NAME[emit_kind_name(n)]
        kind_enum = BLK_ENUM[kind_id]
        text_lit = c_str(n["text"]) if n.get("text") else "0"
        pix = n.get("pix") or {}
        desc_inits.append(
            "{ .id = %d, .kind = %s, .parent = %d, .spec = &spec_%d,"
            " .text = %s, .pix = 0, .pix_w = %d, .pix_h = %d }"
            % (nid, kind_enum, n.get("ce", {}).get("parent", 0),
               nid, text_lit, pix.get("w", 0), pix.get("h", 0)))
    lines.append("")
    lines.append("void blkgen_commit(void) {")
    lines.append("    static const blk_desc descs[] = {")
    for d in desc_inits:
        lines.append("        %s," % d)
    lines.append("    };")
    lines.append("    gui_commit_desc(descs, sizeof(descs) / sizeof(descs[0]));")
    lines.append("}")
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------------
# 主流程
# ---------------------------------------------------------------------------


def write_outputs(tree, nodes, pool, offsets, out_prefix):
    recs = b"".join(serialize_rec(n, offsets[n["id"]]) for n in nodes)
    recs_path = out_prefix + ".recs.bin"
    pool_path = out_prefix + ".strings.bin"
    with open(recs_path, "wb") as f:
        f.write(recs)
    with open(pool_path, "wb") as f:
        f.write(pool)
    return recs_path, pool_path, recs


def stats(nodes, pool):
    text_bytes = sum(len(n.get("text", "").encode("utf-8")) for n in nodes)
    return {
        "blocks": len(nodes),
        "pool_bytes": len(pool),
        "text_bytes": text_bytes,
        "quota": QUOTA,
        "over_blocks": len(nodes) > QUOTA["max_blocks"],
        "over_text": text_bytes > QUOTA["max_text_bytes"],
        "over_pool": len(pool) > QUOTA["max_pool_bytes"],
    }


def main(argv=None):
    ap = argparse.ArgumentParser(description=".blk.json → recs.bin + strings.bin")
    ap.add_argument("blkjson", help="输入 .blk.json")
    ap.add_argument("--out-prefix", help="输出前缀（默认输入文件名去后缀）")
    ap.add_argument("--emit-c", metavar="OUT.c", help="额外产出内置页 C 片段")
    ap.add_argument("--stats", action="store_true", help="打印配额统计")
    ap.add_argument("--patch-json", action="store_true",
                    help="把回填的 text_off 写回输入 .blk.json 的 ce 镜像")
    args = ap.parse_args(argv)

    root, tree = load_tree(args.blkjson)
    nodes = collect_nodes(root)
    pool, offsets = build_pool(nodes)
    verify_pool(pool, offsets, nodes)

    prefix = args.out_prefix
    if not prefix:
        prefix = os.path.splitext(os.path.abspath(args.blkjson))[0]
    recs_path, pool_path, recs = write_outputs(tree, nodes, pool, offsets, prefix)

    if args.emit_c:
        c_text = emit_c(tree, nodes, pool)
        with open(args.emit_c, "w", encoding="utf-8") as f:
            f.write(c_text)

    if args.patch_json:
        for n in nodes:
            n.setdefault("ce", {})["text_off"] = offsets[n["id"]]
        with open(args.blkjson, "w", encoding="utf-8") as f:
            json.dump(tree, f, ensure_ascii=False, indent=2)

    st = stats(nodes, pool)
    if args.stats:
        print("blocks      : %d (quota %d%s)" %
              (st["blocks"], QUOTA["max_blocks"],
               " OVER" if st["over_blocks"] else ""))
        print("text bytes  : %d (quota %d%s)" %
              (st["text_bytes"], QUOTA["max_text_bytes"],
               " OVER" if st["over_text"] else ""))
        print("pool bytes  : %d (quota %d%s)" %
              (st["pool_bytes"], QUOTA["max_pool_bytes"],
               " OVER" if st["over_pool"] else ""))
        print("recs bytes  : %d (%d bytes/rec)" % (len(recs), bc.REC_BYTES))
    print("wrote %s" % recs_path, file=sys.stderr)
    print("wrote %s" % pool_path, file=sys.stderr)

    if st["over_blocks"] or st["over_text"] or st["over_pool"]:
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
