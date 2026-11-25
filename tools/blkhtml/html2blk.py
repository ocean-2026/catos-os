#!/usr/bin/env python3
"""html2blk -- 受控 HTML 子集 → .blk.json（schema catos.blk/1.0）。

用法：
    python3 html2blk.py page.html [--out page.blk.json] [--allow-lossy]
                       [--json] [--title T] [--theme dark|light|hc]

契约：tools/blkhtml/schema.json（字段名/取值/规则 ID 不许改、不许加）。
命中 css_properties.unsupported 的属性逐条输出
`UNSUPPORTED <selector> <prop>`，默认非 0 退出；--allow-lossy 降级为
warning 且退出 0。纯 Python 3 标准库，零第三方依赖。
"""

import argparse
import json
import os
import re
import sys

import blkcommon as bc


# ---------------------------------------------------------------------------
# 诊断收集
# ---------------------------------------------------------------------------


class Diag:
    def __init__(self, allow_lossy=False):
        self.allow_lossy = allow_lossy
        self.items = []

    def add(self, level, selector, prop, msg=""):
        self.items.append({"level": level, "selector": selector,
                           "prop": prop, "msg": msg})

    def unsupported(self, selector, prop, msg=""):
        # schema :: css_properties.policy：默认非 0 退出；lossy 降级 warning。
        level = "WARN" if self.allow_lossy else "UNSUPPORTED"
        self.add(level, selector, prop, msg)

    @property
    def hard_fail(self):
        return any(i["level"] == "UNSUPPORTED" for i in self.items)

    def lines(self):
        out = []
        for i in self.items:
            tag = i["level"]
            msg = (" %s" % i["msg"]) if i["msg"] else ""
            out.append("%s %s %s%s" % (tag, i["selector"], i["prop"], msg))
        return out


# ---------------------------------------------------------------------------
# CSS 解析
# ---------------------------------------------------------------------------

SPACING = set(bc.schema()["allowed_spacing"]["padding"])
GAP_ALLOWED = set(bc.schema()["allowed_spacing"]["gap"])
MARGIN_ALLOWED = set(bc.schema()["allowed_spacing"]["margin"])

# 裸文本里只要出现即判 unsupported 的模式（schema :: css_properties.unsupported）
RAW_UNSUPPORTED = [
    (r"float\s*:", "float"),
    (r"position\s*:\s*fixed", "position:fixed"),
    (r"position\s*:\s*sticky", "position:sticky"),
    (r"display\s*:\s*grid", "display:grid"),
    (r"inline-block", "inline-block"),
    (r"transform\s*:", "transform"),
    (r"transition\s*:", "transition"),
    (r"animation\s*:", "animation"),
    (r"filter\s*:", "filter"),
    (r"backdrop-filter\s*:", "backdrop-filter"),
    (r"background-image\s*:", "background-image"),
    (r"url\(", "url()"),
    (r"gradient", "gradient"),
    (r"rgba\(", "rgba()"),
    (r"hsl\(", "hsl()"),
    (r"mix-blend-mode\s*:", "mix-blend-mode"),
    (r"clip-path\s*:", "clip-path"),
    (r"mask\s*:", "mask"),
    (r"writing-mode\s*:", "writing-mode"),
    (r"direction\s*:\s*rtl", "direction:rtl"),
    (r"::before|::after", "pseudo-element"),
    (r"var\(", "css variable"),
    (r"text-align\s*:", "text-align"),
    (r"font-family\s*:", "font-family"),
    (r"font-weight\s*:", "font-weight"),
    (r"font-style\s*:", "font-style"),
]

UNSUPPORTED_TAGS = {"table", "li", "iframe", "canvas", "video", "svg", "ul", "ol"}

# 结构性标签：不进块树，也不报 UNSUPPORTED。
STRUCTURAL = {"html", "head", "body", "meta", "title", "link", "script",
              "style", "noscript"}

# HTML void 元素：入栈即出栈，后续兄弟不会被错挂成其子树（B5）。
VOID = {"input", "hr", "img", "br"}

TAG_KIND = {"div": "container", "span": "text", "button": "button",
            "input": "input", "img": "image", "hr": "divider", "br": "spacer"}


def split_decl(decl):
    """'a:1; b:2' → [('a','b'),...]，去注释/空白。"""
    out = []
    decl = re.sub(r"/\*.*?\*/", "", decl)
    for part in decl.split(";"):
        part = part.strip()
        if not part:
            continue
        if ":" not in part:
            continue
        k, v = part.split(":", 1)
        out.append((k.strip().lower(), v.strip()))
    return out


def parse_style_rules(style_text):
    """<style> 内受限 class 规则：{'.cls': {prop: value}}。"""
    rules = {}
    style_text = re.sub(r"/\*.*?\*/", "", style_text)
    for sel, body in re.findall(r"\.([A-Za-z0-9_-]+)\s*\{([^}]*)\}", style_text):
        rules[sel] = dict(split_decl(body))
    return rules


def px_value(v, diag, selector, prop):
    """解析 '12px' → int；'100%'/'auto' 特殊值原样返回。"""
    v = v.strip()
    if v in ("100%", "auto", ""):
        return v
    m = re.fullmatch(r"(\d+)\s*px", v)
    if m:
        return int(m.group(1))
    diag.unsupported(selector, prop, "want px|100%|auto, got %r" % v)
    return None


def parse_spacing(v, allowed, diag, selector, prop):
    """解析 padding/margin 简写 → {t,r,b,l}，值必须属于 allowed 档。"""
    vals = [x.strip() for x in v.split()]
    nums = []
    for x in vals:
        x = x.replace("px", "").strip()
        m = re.fullmatch(r"(\d+)", x)
        if not m:
            diag.unsupported(selector, prop, "bad spacing %r" % x)
            return None
        nums.append(int(m.group(1)))
    if not nums:
        return None
    if len(nums) == 1:
        t = r = b = l = nums[0]
    elif len(nums) == 2:
        t = b = nums[0]; l = r = nums[1]
    elif len(nums) == 3:
        t = nums[0]; l = r = nums[1]; b = nums[2]
    else:
        t, r, b, l = nums[:4]
    for side, val in (("t", t), ("r", r), ("b", b), ("l", l)):
        if val not in allowed:
            diag.unsupported(selector, prop,
                             "spacing %d not in %s" % (val, sorted(allowed)))
    return {"t": t, "r": r, "b": b, "l": l}


# ---------------------------------------------------------------------------
# 中间树构建（html.parser）
# ---------------------------------------------------------------------------


class El:
    __slots__ = ("tag", "classes", "style", "text", "children", "data",
                 "attrs")

    def __init__(self, tag, attrs):
        self.tag = tag
        self.classes = []
        self.style = {}
        self.text = ""
        self.children = []
        self.data = {}
        self.attrs = dict(attrs)


class TreeBuilder:
    def __init__(self, diag, class_rules):
        self.diag = diag
        self.class_rules = class_rules
        self.root = El("body", [])
        self.stack = [self.root]
        self.style_buf = []
        self.in_style = False

    # -- parser hooks -------------------------------------------------------
    def handle_starttag(self, tag, attrs):
        tag = tag.lower()
        if tag == "style":
            self.in_style = True
            self.style_buf = []
            return
        if tag in STRUCTURAL:
            return
        if tag in UNSUPPORTED_TAGS:
            self.diag.unsupported(tag, "<tag>", "tag not in html_subset")
            return
        if tag not in TAG_KIND:
            self.diag.unsupported(tag, "<tag>", "unknown tag")
            return
        el = El(tag, attrs)
        cls = el.attrs.get("class", "")
        el.classes = [c for c in cls.split() if c]
        for c in el.classes:
            if c not in self.class_rules:
                self.diag.unsupported("." + c, "UNSUPPORTED_CLASS",
                                      "class not declared in <style>")
        # 级联：class 规则先，内联 style 覆盖
        for c in el.classes:
            el.style.update(self.class_rules.get(c, {}))
        inline = el.attrs.get("style", "")
        el.style.update(dict(split_decl(inline)))
        # data-blk-* 覆盖
        for k, val in el.attrs.items():
            if k.startswith("data-blk-"):
                el.data[k[len("data-blk-"):]] = val
        self.stack[-1].children.append(el)
        if tag not in VOID:
            self.stack.append(el)

    def handle_endtag(self, tag):
        tag = tag.lower()
        if tag == "style":
            self.in_style = False
            return
        if tag in UNSUPPORTED_TAGS or tag not in TAG_KIND:
            return
        # 弹回对应标签（容错：不强制严格配对）
        for i in range(len(self.stack) - 1, 0, -1):
            if self.stack[i].tag == tag:
                del self.stack[i:]
                break

    def handle_data(self, data):
        if self.in_style:
            self.style_buf.append(data)
            return
        text = data
        if self.stack:
            self.stack[-1].text += text

    def close_style(self):
        if self.style_buf:
            self.class_rules.update(parse_style_rules("".join(self.style_buf)))


# ---------------------------------------------------------------------------
# 样式 → schema 节点字段
# ---------------------------------------------------------------------------

ALIGN_MAIN = {"flex-start": "start", "center": "center",
              "flex-end": "end", "space-between": "space_between"}
ALIGN_CROSS = {"flex-start": "start", "center": "center",
               "flex-end": "end", "stretch": "stretch"}
OVF_MAP = {"hidden": "clip", "visible": "visible",
           "auto": "scroll", "scroll": "scroll"}


def inspect_raw(style_text, selector, diag):
    for pat, name in RAW_UNSUPPORTED:
        if re.search(pat, style_text):
            diag.unsupported(selector, name)


def apply_style(el, node, diag, selector):
    st = el.style
    inspect_raw(";".join("%s:%s" % kv for kv in st.items()), selector, diag)

    # display / flex-direction → flow
    display = st.get("display", "block")
    if display not in ("flex", "block", "none"):
        diag.unsupported(selector, "display", "want flex|block|none")
    if display == "none":
        node["visible"] = False
    direction = st.get("flex-direction", "column")
    if direction not in ("row", "column"):
        diag.unsupported(selector, "flex-direction", "want row|column")

    if el.tag == "div":
        if st.get("position") == "absolute":
            flow = "free"
        elif display == "flex":
            flow = direction
        else:
            flow = "column"
        node["layout"] = {"flow": flow}

    # 主轴 / 交叉轴对齐
    if "justify-content" in st:
        v = st["justify-content"]
        if v in ALIGN_MAIN:
            node["layout"]["align_main"] = ALIGN_MAIN[v]
        else:
            diag.unsupported(selector, "justify-content", v)
    if "align-items" in st:
        v = st["align-items"]
        if v in ALIGN_CROSS:
            node["layout"]["align_cross"] = ALIGN_CROSS[v]
        else:
            diag.unsupported(selector, "align-items", v)

    # gap（接受 N 与 Npx；档位仍须属 allowed_spacing）
    if "gap" in st:
        m = re.fullmatch(r"(\d+)\s*(?:px)?", st["gap"].strip())
        g = int(m.group(1)) if m else None
        if g is not None and g in GAP_ALLOWED:
            node["layout"]["gap"] = g
        else:
            diag.unsupported(selector, "gap", "%s not in %s" %
                             (st["gap"], sorted(GAP_ALLOWED)))

    # padding / margin
    if "padding" in st:
        p = parse_spacing(st["padding"], SPACING, diag, selector, "padding")
        if p:
            node["padding"] = p
    for side in ("top", "right", "bottom", "left"):
        key = "padding-" + side
        if key in st:
            val = parse_spacing(st[key], SPACING, diag, selector, key)
            if val:
                pad = node.setdefault("padding", {"t": 0, "r": 0, "b": 0, "l": 0})
                pad[side[0]] = val[side[0]]
    if "margin" in st:
        m = parse_spacing(st["margin"], MARGIN_ALLOWED, diag, selector, "margin")
        if m:
            node["margin"] = m
    for side in ("top", "right", "bottom", "left"):
        key = "margin-" + side
        if key in st:
            val = parse_spacing(st[key], MARGIN_ALLOWED, diag, selector, key)
            if val:
                mg = node.setdefault("margin", {"t": 0, "r": 0, "b": 0, "l": 0})
                mg[side[0]] = val[side[0]]

    # 声明宽高
    req = {"w": -1, "h": -1}
    for axis, prop in (("w", "width"), ("h", "height")):
        if prop in st:
            v = px_value(st[prop], diag, selector, prop)
            if isinstance(v, int):
                req[axis] = v
            elif v == "100%":
                req[axis] = -2  # 标记：填满父内容框
    node["req"] = req

    # 定位：left/top/right/bottom → px / pct
    pos = {}
    for side, prop in (("x", "left"), ("y", "top"), ("x", "right"),
                        ("y", "bottom")):
        if prop in st:
            v = st[prop].strip()
            m = re.fullmatch(r"(\d+)%", v)
            if m:
                pct = node.setdefault("pct", {})
                pct[side] = int(int(m.group(1)) * 1000 / 100)
            else:
                pv = px_value(v, diag, selector, prop)
                if isinstance(pv, int):
                    px = node.setdefault("px", {})
                    px[side] = pv
            pos[prop] = v

    # 颜色：保留角色名或 #RRGGBB 原样存储，预览期按主题解析。
    def store_color(key, val):
        if val in bc.ROLE_NAMES or re.fullmatch(r"#[0-9a-fA-F]{6}", val):
            node[key] = val
        else:
            diag.unsupported(selector, key, "bad color %r" % val)

    if "background" in st or "background-color" in st:
        store_color("fill", st.get("background-color", st.get("background")))
    if "color" in st:
        store_color("fg", st["color"])

    # 描边
    stroke = {"w": 0, "sides": ["L", "R", "T", "B"], "color": None}
    if "border" in st:
        parts = st["border"].split()
        for p in parts:
            if re.fullmatch(r"[012]", p) or re.fullmatch(r"(\d+)px", p):
                stroke["w"] = int(re.search(r"\d+", p).group(0))
            elif p in ("solid", "none", "dashed", "dotted", "double"):
                continue
            elif p in bc.ROLE_NAMES or re.fullmatch(r"#[0-9a-fA-F]{6}", p):
                stroke["color"] = p
            else:
                diag.unsupported(selector, "border-color", p)
        if stroke["w"] not in (0, 1, 2):
            diag.unsupported(selector, "border-width", "want 0|1|2")
    if "border-width" in st:
        m = re.fullmatch(r"([012])", st["border-width"].strip())
        if m:
            stroke["w"] = int(m.group(1))
        else:
            diag.unsupported(selector, "border-width", st["border-width"])
    if "border-color" in st:
        if st["border-color"] in bc.ROLE_NAMES or \
                re.fullmatch(r"#[0-9a-fA-F]{6}", st["border-color"]):
            stroke["color"] = st["border-color"]
        else:
            diag.unsupported(selector, "border-color", st["border-color"])
    if stroke["w"] > 0:
        node["stroke"] = stroke

    # 圆角：档 0/8/12/16/24，四值逐角
    if "border-radius" in st:
        rv = st["border-radius"].strip()
        if rv.endswith("%"):
            diag.unsupported(selector, "border-radius", "percentage radius")
        else:
            nums = re.findall(r"(\d+)", rv)
            ladder = {0, 8, 12, 16, 24}
            rad = {}
            ok = True
            if len(nums) == 1:
                if int(nums[0]) in ladder:
                    rad = {c: int(nums[0]) for c in ("tl", "tr", "br", "bl")}
                else:
                    ok = False
            elif len(nums) == 4:
                for c, x in zip(("tl", "tr", "br", "bl"), nums):
                    if int(x) in ladder:
                        rad[c] = int(x)
                    else:
                        ok = False
            else:
                ok = False
            if not ok:
                diag.unsupported(selector, "border-radius",
                                 "ladder 0/8/12/16/24, got %r" % rv)
            else:
                node["radius"] = rad
                node["radius_mode"] = "per_corner"

    # 阴影
    if "box-shadow" in st:
        v = st["box-shadow"].strip()
        if v == "none":
            node["shadow"] = {"mode": "none", "sides": []}
        elif v in ("contact", "dual"):
            node["shadow"] = {"mode": v, "sides": ["L", "R", "T", "B"]}
        else:
            diag.unsupported(selector, "box-shadow", v)

    # 溢出
    if "overflow" in st:
        v = st["overflow"].strip()
        if v in OVF_MAP:
            node["overflow"] = OVF_MAP[v]
        else:
            diag.unsupported(selector, "overflow", v)

    # 字号 → scale
    if "font-size" in st:
        v = st["font-size"].strip()
        if v in ("12px", "12"):
            node.setdefault("font", {})["scale"] = 1
        elif v in ("16px", "16"):
            node.setdefault("font", {})["scale"] = 2
        else:
            diag.unsupported(selector, "font-size",
                             "want 12|16, got %r" % v)
    if "line-height" in st:
        v = st["line-height"].strip()
        if v not in ("16", "16px", "32", "32px"):
            diag.unsupported(selector, "line-height", v)
    if "opacity" in st:
        if st["opacity"].strip() not in ("1", "1.0", "100%"):
            diag.unsupported(selector, "opacity", st["opacity"])
    if "z-index" in st:
        m = re.fullmatch(r"(-?\d+)", st["z-index"].strip())
        if m:
            node["z"] = int(m.group(1))
        else:
            diag.unsupported(selector, "z-index", st["z-index"])

    # data-blk-* 显式覆盖（schema :: html_subset.attributes.data-blk）
    if "layer" in el.data:
        if el.data["layer"] in bc.LAYER_FROM_NAME:
            node["layer"] = el.data["layer"]
        else:
            diag.unsupported(selector, "data-blk-layer", el.data["layer"])
    if "anchor" in el.data:
        anchors = [a.strip() for a in re.split(r"[|,]", el.data["anchor"]) if a.strip()]
        for a in anchors:
            if a not in bc.ANCHOR_BITS:
                diag.unsupported(selector, "data-blk-anchor", a)
        node["anchor"] = anchors
    if "overflow" in el.data:
        if el.data["overflow"] in OVF_MAP:
            node["overflow"] = OVF_MAP[el.data["overflow"]]
        else:
            diag.unsupported(selector, "data-blk-overflow", el.data["overflow"])
    if "drag" in el.data:
        if el.data["drag"] in ("none", "title", "full"):
            node["drag"] = el.data["drag"]
        else:
            diag.unsupported(selector, "data-blk-drag", el.data["drag"])
    if "drag-bounds" in el.data:
        if el.data["drag-bounds"] in ("free", "parent", "screen"):
            node["drag_bounds"] = el.data["drag-bounds"]
        else:
            diag.unsupported(selector, "data-blk-drag-bounds",
                             el.data["drag-bounds"])
    if "resize" in el.data:
        dirs = [d.strip() for d in re.split(r"[|,]", el.data["resize"]) if d.strip()]
        bad = [d for d in dirs if d not in ("L", "R", "T", "B")]
        if bad:
            diag.unsupported(selector, "data-blk-resize", ",".join(bad))
        else:
            edge = 6
            if "resize-edge" in el.data:
                m = re.fullmatch(r"(\d+)", el.data["resize-edge"].strip())
                edge = int(m.group(1)) if m else edge
            node["resize"] = {"dir": dirs, "edge": edge, "min_w": 24,
                             "min_h": 24, "max_w": 0, "max_h": 0, "step": 0}
    elif "resize-edge" in el.data:
        diag.unsupported(selector, "data-blk-resize-edge",
                         "needs data-blk-resize")
    if "bleed" in el.data:
        node["bleed"] = el.data["bleed"].strip().lower() in ("true", "1", "yes")
    if "z" in el.data:
        m = re.fullmatch(r"(-?\d+)", el.data["z"].strip())
        if m:
            node["z"] = int(m.group(1))
        else:
            diag.unsupported(selector, "data-blk-z", el.data["z"])
    if "text-overflow" in el.data:
        if el.data["text-overflow"] in ("ellipsis", "clip"):
            node["text_overflow"] = el.data["text-overflow"]
        else:
            diag.unsupported(selector, "data-blk-text-overflow",
                             el.data["text-overflow"])
    if "role" in el.data:
        role = el.data["role"]
        if role in bc.ROLE_NAMES or re.fullmatch(r"#[0-9a-fA-F]{6}", role):
            node["fill"] = role
        else:
            diag.unsupported(selector, "data-blk-role", role)


# ---------------------------------------------------------------------------
# 布局：度量 + 落矩形（8pt 量化）
# ---------------------------------------------------------------------------


def natural_size(el, node, ctx):
    """叶子自然尺寸；容器由聚合得到。"""
    scale = (node.get("font") or {}).get("scale", 1)
    tag = el.tag
    if tag in ("text", "span"):
        return bc.snap8(bc.text_width(el.text.strip(), scale)), 16 * scale
    if tag == "button":
        return bc.snap8(bc.text_width(el.text.strip(), scale)), 24
    if tag == "input":
        return 80, 24
    if tag == "image":
        w = int(el.attrs.get("width", 64))
        h = int(el.attrs.get("height", 64))
        return bc.snap8(w), bc.snap8(h)
    if tag == "divider":
        return 0, 1
    if tag == "spacer":
        return ctx.get("spacer_w", 16), ctx.get("spacer_h", 16)
    return 0, 0


def build_node(el, parent_node, parent_content, ctx, diag, id_counter):
    selector = ".".join(el.classes) or el.tag
    node = {"id": id_counter[0]}
    id_counter[0] += 1

    # 种类映射
    if el.tag == "div":
        kind = "column"  # 容器默认；free/row 在 layout 里体现
    elif el.tag in ("span", "text"):
        kind = "text"
    else:
        kind = TAG_KIND[el.tag]
    node["kind"] = kind
    node.setdefault("layer", "work")
    node.setdefault("visible", True)
    node.setdefault("focusable", False)
    node.setdefault("inert", False)

    apply_style(el, node, diag, selector)

    # B7：容器 kind 由 flex-direction 决定（row→row；column/stack/free→column）。
    # 内核布局按 kind 判主轴，kind 与 flow 不一致会把 row 摆成竖排。
    if el.tag == "div":
        flow = (node.get("layout") or {}).get("flow", "column")
        node["kind"] = "row" if flow == "row" else "column"

    # 文本 / 位图
    text = el.text.strip() if el.tag in ("span", "button", "input") else \
        el.text.strip()
    if el.tag == "input":
        text = el.attrs.get("value") or el.attrs.get("placeholder") or ""
    if text:
        node["text"] = text
    if el.tag == "image":
        node["pix"] = {"path": el.attrs.get("src", ""),
                        "w": int(el.attrs.get("width", 64)),
                        "h": int(el.attrs.get("height", 64)), "res": -1}

    # 递归子节点
    children = [c for c in el.children if c.tag in TAG_KIND]
    child_nodes = []
    for c in children:
        cn = build_node(c, node, None, ctx, diag, id_counter)
        child_nodes.append(cn)
    node["children"] = child_nodes

    # 度量容器自然尺寸
    flow = (node.get("layout") or {}).get("flow", "column")
    pad = node.get("padding", 0)
    if isinstance(pad, int):
        pt = pr = pb = pl = pad
    else:
        pt = pad.get("t", 0); pr = pad.get("r", 0)
        pb = pad.get("b", 0); pl = pad.get("l", 0)

    if child_nodes:
        if flow == "row":
            nat_w = pl + pr + sum(c["rect"]["w"] for c in child_nodes)
            nat_h = pt + pb + max(c["rect"]["h"] for c in child_nodes)
        elif flow in ("column", "stack", "free"):
            nat_w = pl + pr + max(c["rect"]["w"] for c in child_nodes)
            nat_h = pt + pb + sum(c["rect"]["h"] for c in child_nodes)
        else:
            nat_w = nat_h = 0
    else:
        nat_w, nat_h = natural_size(el, node, ctx)

    req = node.get("req", {"w": -1, "h": -1})
    cw = parent_content["w"] if parent_content else ctx["frame_w"]
    ch = parent_content["h"] if parent_content else ctx["frame_h"]
    w = req["w"] if req["w"] >= 0 else (cw if req["w"] == -2 else nat_w)
    h = req["h"] if req["h"] >= 0 else (ch if req["h"] == -2 else nat_h)
    node["rect"] = {"x": 0, "y": 0, "w": bc.snap8(w), "h": bc.snap8(h)}
    return node


def place_children(node, diag):
    """按 layout.flow 把子块落进父内容框；8pt 量化后写绝对 rect。"""
    pad = node.get("padding", 0)
    if isinstance(pad, int):
        pt = pr = pb = pl = pad
    else:
        pt = pad.get("t", 0); pr = pad.get("r", 0)
        pb = pad.get("b", 0); pl = pad.get("l", 0)
    r = node["rect"]
    cx, cy = r["x"] + pl, r["y"] + pt
    cw = max(0, r["w"] - pl - pr)
    ch = max(0, r["h"] - pt - pb)
    children = node.get("children") or []
    if not children:
        return

    layout = node.get("layout") or {}
    flow = layout.get("flow", "column")
    gap = layout.get("gap", 0)
    amain = layout.get("align_main", "start")
    across = layout.get("align_cross", "start")

    if flow == "free":
        # B1：FREE 子块落位后必须继续递归后代，子块用相对本块内容矩形的
        # 锚点/百分比求解；否则整棵子树停在 (0,0)。
        for c in children:
            place_free(c, cx, cy, cw, ch)
            place_children(c, diag)
        return

    if flow == "row":
        # 对齐口径照抄 kernel_v25/src/gui/layout.c :: layout_row：
        # 主轴 x 按 align_main 起排；space_between 在子块间平分剩余空间；
        # 交叉轴 y 按 align_cross 落到父内容框。
        total_w = sum(c["rect"]["w"] for c in children) + gap * (len(children) - 1)
        x0 = cx
        if amain == "center":
            x0 = cx + (cw - total_w) // 2
        elif amain == "end":
            x0 = cx + cw - total_w
        slot = 0
        if amain == "space_between" and len(children) > 1:
            slot = (cw - total_w) // (len(children) - 1)
        x = x0
        for c in children:
            y = cy
            if across == "center":
                y = cy + (ch - c["rect"]["h"]) // 2
            elif across == "end":
                y = cy + ch - c["rect"]["h"]
            elif across == "stretch":
                c["rect"]["h"] = ch
            c["rect"]["x"] = x
            c["rect"]["y"] = y
            x += c["rect"]["w"] + gap + slot
    else:  # column / stack
        # 对齐口径照抄 layout.c :: layout_column；stack 层叠不沿主轴推进。
        total_h = sum(c["rect"]["h"] for c in children) + gap * (len(children) - 1)
        y0 = cy
        if amain == "center":
            y0 = cy + (ch - total_h) // 2
        elif amain == "end":
            y0 = cy + ch - total_h
        slot = 0
        if amain == "space_between" and len(children) > 1 and flow != "stack":
            slot = (ch - total_h) // (len(children) - 1)
        y = y0
        for c in children:
            x = cx
            if across == "center":
                x = cx + (cw - c["rect"]["w"]) // 2
            elif across == "end":
                x = cx + cw - c["rect"]["w"]
            elif across == "stretch":
                c["rect"]["w"] = cw
            c["rect"]["x"] = x
            c["rect"]["y"] = y
            if flow != "stack":
                y += c["rect"]["h"] + gap + slot

    for c in children:
        place_children(c, diag)


def place_free(child, cx, cy, cw, ch):
    """layout_free_child 口径：锚点/百分比/px 求解。"""
    px = child.get("px") or {}
    pct = child.get("pct") or {}
    w, h = child["rect"]["w"], child["rect"]["h"]
    anchor = child.get("anchor") or []
    x, y = cx + px.get("x", 0), cy + px.get("y", 0)
    if "R" in anchor:
        x = cx + cw - w - px.get("x", 0)
    elif "HCENTER" in anchor:
        x = cx + (cw - w) // 2 + px.get("x", 0)
    if "B" in anchor:
        y = cy + ch - h - px.get("y", 0)
    elif "VCENTER" in anchor:
        y = cy + (ch - h) // 2 + px.get("y", 0)
    if pct.get("x"):
        x = cx + cw * pct["x"] // 1000
    if pct.get("y"):
        y = cy + ch * pct["y"] // 1000
    child["rect"]["x"] = x
    child["rect"]["y"] = y


def apply_flush(root, diag):
    """schema :: flush_rule：贴边侧 radius/stroke/shadow 归零，共享边 1px。"""
    def walk(node):
        for c in node.get("children") or []:
            sides = bc.touch_sides(c, node)
            if sides:
                c["flush_sides"] = sides
                rad = c.get("radius")
                if rad:
                    for s in sides:
                        for corner in bc.SIDE_CORNERS[s]:
                            rad[corner] = 0
                stroke = c.get("stroke")
                if stroke:
                    stroke["sides"] = [s for s in stroke["sides"]
                                       if s not in sides]
                    if not stroke["sides"]:
                        c.pop("stroke", None)
                shadow = c.get("shadow")
                if shadow:
                    shadow["sides"] = [s for s in shadow.get("sides", [])
                                       if s not in sides]
            walk(c)
    walk(root)


def fill_ce_mirror(root):
    """写出每个节点的 ce 九字段镜像（text_off/pix_off 由 blk2bin 回填）。"""
    def walk(node, parent_id):
        kind_id = bc.KIND_FROM_NAME[node["kind"]]
        layer_id = bc.LAYER_FROM_NAME.get(node.get("layer", "work"), 200)
        req = node.get("req", {"w": -1, "h": -1})
        pix = node.get("pix") or {}
        node["ce"] = {
            "id": node["id"],
            "parent": parent_id,
            "kind": kind_id,
            "layer": layer_id,
            "req_w": req["w"] if req["w"] >= 0 else -1,
            "req_h": req["h"] if req["h"] >= 0 else -1,
            "text_off": -1,
            "pix_off": -1,
            "pix_w": pix.get("w", 0),
            "pix_h": pix.get("h", 0),
            "res": pix.get("res", -1),
        }
        for c in node.get("children") or []:
            walk(c, node["id"])
    walk(root, 0)


# ---------------------------------------------------------------------------
# 主流程
# ---------------------------------------------------------------------------


def convert(html_text, title, theme, frame_w=800, frame_h=600,
            allow_lossy=False):
    diag = Diag(allow_lossy)
    style_text = "\n".join(re.findall(
        r"<style[^>]*>(.*?)</style>", html_text, re.S | re.I))
    class_rules = parse_style_rules(style_text)
    builder = TreeBuilder(diag, class_rules)

    from html.parser import HTMLParser

    class P(HTMLParser):
        def handle_starttag(self, tag, attrs):
            builder.handle_starttag(tag, attrs)

        def handle_endtag(self, tag):
            builder.handle_endtag(tag)

        def handle_data(self, data):
            builder.handle_data(data)

    parser = P(convert_charrefs=True)
    parser.feed(html_text)
    builder.close_style()

    # body 下的直接子元素作为根节点子树
    body = builder.root
    ctx = {"frame_w": frame_w, "frame_h": frame_h}
    id_counter = [1]

    root = {"id": 0, "kind": "root", "layer": "work", "visible": True,
            "rect": {"x": 0, "y": 0, "w": frame_w, "h": frame_h},
            "children": []}

    # 先建中间节点树（含自然尺寸）
    el_children = [c for c in body.children if c.tag in TAG_KIND]
    built = []
    for el in el_children:
        built.append(build_node(el, root, {"w": frame_w, "h": frame_h},
                                ctx, diag, id_counter))
    root["children"] = built

    # 根尺寸 100% 子项填满
    for c in root["children"]:
        req = c.get("req", {})
        if req.get("w") == -2:
            c["rect"]["w"] = frame_w
        if req.get("h") == -2:
            c["rect"]["h"] = frame_h

    place_children(root, diag)
    apply_flush(root, diag)
    fill_ce_mirror(root)

    tree = {"schema": "catos.blk/1.0", "title": title, "theme": theme,
            "root": root}
    return tree, diag


def main(argv=None):
    ap = argparse.ArgumentParser(description="受控 HTML 子集 → .blk.json")
    ap.add_argument("html", help="输入 HTML 文件")
    ap.add_argument("--out", help="输出 .blk.json 路径")
    ap.add_argument("--allow-lossy", action="store_true",
                    help="UNSUPPORTED 降级为 warning，退出 0")
    ap.add_argument("--json", action="store_true",
                    help="机读 JSON 输出（含 diagnostics）")
    ap.add_argument("--title", default="CatOS page")
    ap.add_argument("--theme", choices=["dark", "light", "hc"], default="dark")
    ap.add_argument("--frame-w", type=int, default=800)
    ap.add_argument("--frame-h", type=int, default=600)
    args = ap.parse_args(argv)

    with open(args.html, "r", encoding="utf-8") as f:
        html_text = f.read()

    tree, diag = convert(html_text, args.title, args.theme,
                         args.frame_w, args.frame_h, args.allow_lossy)

    out = args.out
    if not out:
        out = os.path.splitext(args.html)[0] + ".blk.json"
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, "w", encoding="utf-8") as f:
        json.dump(tree, f, ensure_ascii=False, indent=2)

    if args.json:
        print(json.dumps({"output": out,
                          "diagnostics": diag.items,
                          "blk": tree}, ensure_ascii=False, indent=2))
    else:
        for line in diag.lines():
            print(line, file=sys.stderr)
        print("wrote %s" % out, file=sys.stderr)

    return 1 if diag.hard_fail else 0


if __name__ == "__main__":
    sys.exit(main())
