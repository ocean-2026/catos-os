"""blkhtml 内部共享库（非 CLI 入口）。

四件交付工具（html2blk / blk2bin / preview / validate）只认 schema.json 这
一份冻结契约；本模块集中放：契约常量、令牌调色板、font_ascii.inc 解析、
与 decl_text_width 一致的文本度量、贴边（flush）判定。

纯 Python 3 标准库，零第三方依赖。
"""

import json
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
SCHEMA_PATH = os.path.join(HERE, "schema.json")

# ---------------------------------------------------------------------------
# 契约加载
# ---------------------------------------------------------------------------


def load_schema():
    with open(SCHEMA_PATH, "r", encoding="utf-8") as f:
        return json.load(f)


_SCHEMA = None


def schema():
    global _SCHEMA
    if _SCHEMA is None:
        _SCHEMA = load_schema()
    return _SCHEMA


# ---------------------------------------------------------------------------
# 枚举映射（与 schema.json kind_enum / layer_enum / anchor_bits 一一对应）
# ---------------------------------------------------------------------------

KIND_FROM_NAME = {
    "root": 0, "column": 1, "row": 2, "stack": 3, "text": 4, "rect": 5,
    "image": 6, "surface": 7, "spacer": 8, "divider": 9, "canvas": 10,
    "free": 11, "button": 12, "input": 13, "term": 14,
}

KIND_FROM_ID = {v: k for k, v in KIND_FROM_NAME.items()}

LAYER_FROM_NAME = {
    "bg": 0, "content": 100, "work": 200,
    "chrome": 3000, "overlay": 4000, "toast": 5000,
}

ANCHOR_BITS = {"L": 1, "R": 2, "T": 4, "B": 8, "HCENTER": 16, "VCENTER": 32}

OVF_FROM_NAME = {"clip": 0, "visible": 1, "scroll": 2}

# 配额（schema.json :: quotas，对应 kernel decl.h）
QUOTAS = {"max_blocks": 256, "max_depth": 16,
          "max_text_bytes": 4096, "max_pool_bytes": 32768}

# ---------------------------------------------------------------------------
# CE 线上记录布局（40 字节/条，schema :: ce_blk_rec_t.byte_layout）
#
# 步长以 sizeof(ce_blk_rec_t) 为准（decl.c:329-330 按 n * sizeof 拷贝）：
#   off  0  i32  id
#   off  4  i32  parent
#   off  8  u8   kind
#   off  9  u8   pad0      必须写 0
#   off 10  u16  layer
#   off 12  i32  req_w
#   off 16  i32  req_h
#   off 20  i32  text_off   -1 = 无文本
#   off 24  i32  pix_off    恒 -1（validate 拒绝内联像素）
#   off 28  i32  pix_w
#   off 32  i32  pix_h
#   off 36  i32  res        -1 = 无图像资源
# ---------------------------------------------------------------------------

REC_FMT = "<i i B B H i i i i i i i"
REC_BYTES = 40

# ---------------------------------------------------------------------------
# 调色板（schema :: token_palette，逐字取自 kernel tokens.c）
# ---------------------------------------------------------------------------

ROLE_NAMES = ("fg", "bg", "panel", "bar", "border", "line", "accent")


def palette(theme):
    pal = schema()["token_palette"]
    if theme not in pal:
        raise ValueError("unknown theme %r (want dark|light|hc)" % theme)
    return pal[theme]


def resolve_color(spec, theme):
    """把 fill/fg/accent 声明解析成 #RRGGBB。

    spec: 角色名（fg/bg/panel/bar/border/line/accent）或 #RRGGBB；
    缺省回退由调用方决定，这里 None 原样返回。
    """
    if spec is None:
        return None
    s = str(spec).strip()
    if s.startswith("#"):
        if re.fullmatch(r"#[0-9a-fA-F]{6}", s):
            return s.upper()[1:]
        raise ValueError("bad color %r" % s)
    pal = palette(theme)
    if s in pal:
        return pal[s]
    raise ValueError("unknown color role %r" % s)


def hex_rgb(hh):
    return (int(hh[0:2], 16), int(hh[2:4], 16), int(hh[4:6], 16))


def mix_black(hex_color, kpermil):
    """paint.c mix_black：底色按千分比压黑（180=黑18%，100=黑10%）。"""
    r, g, b = hex_rgb(hex_color)
    r = r * (1000 - kpermil) // 1000
    g = g * (1000 - kpermil) // 1000
    b = b * (1000 - kpermil) // 1000
    return "%02X%02X%02X" % (r, g, b)


# ---------------------------------------------------------------------------
# font_ascii.inc 解析（8x16，16 字节/字形，MSB 左；0x20..0x7E）
# ---------------------------------------------------------------------------

_FONT_CACHE = {}


def _kernel_root_candidates():
    """内核根目录候选，按优先级：
    ① 环境变量 ``CATOS_ROOT`` → ``$CATOS_ROOT/kernel_v25``；
    ② 仓库内相对路径（``tools/blkhtml`` → 仓库根下的 ``kernel_v25``）；
    ③ 旧的兄弟树相对路径兜底（``../../../catos-os/kernel_v25``）。
    """
    cands = []
    env = os.environ.get("CATOS_ROOT")
    if env:
        cands.append(os.path.join(env, "kernel_v25"))
    cands.append(os.path.abspath(os.path.join(HERE, "..", "..", "kernel_v25")))
    cands.append(os.path.abspath(os.path.join(
        HERE, "..", "..", "..", "catos-os", "kernel_v25")))
    return cands


def kernel_inc_dir():
    """解析内核 ``include`` 目录；找不到返回 None。"""
    for root in _kernel_root_candidates():
        p = os.path.join(root, "include")
        if os.path.isdir(p):
            return os.path.abspath(p)
    return None


def font_path(override=None):
    if override:
        return override
    for root in _kernel_root_candidates():
        p = os.path.join(root, "src", "font_ascii.inc")
        if os.path.exists(p):
            return os.path.abspath(p)
    return None


def load_font(override=None):
    """返回 dict {ord: [16 bytes]}。解析失败抛异常，不许用系统字体近似。"""
    key = override or "default"
    if key in _FONT_CACHE:
        return _FONT_CACHE[key]
    path = font_path(override)
    if not path or not os.path.exists(path):
        tried = [os.path.join(r, "src", "font_ascii.inc")
                 for r in _kernel_root_candidates()]
        raise FileNotFoundError(
            "font_ascii.inc not found; searched: " + "; ".join(tried))
    glyphs = {}
    data = open(path, "r", encoding="utf-8", errors="replace").read()
    groups = re.findall(r"\{([^}]*)\}", data)
    for idx, grp in enumerate(groups):
        bytes_vals = [int(x, 16) for x in re.findall(r"0x[0-9A-Fa-f]{2}", grp)]
        if len(bytes_vals) == 16:
            glyphs[0x20 + idx] = bytes_vals
    if len(glyphs) < 95:
        raise ValueError("font_ascii.inc parse: only %d glyphs" % len(glyphs))
    _FONT_CACHE[key] = glyphs
    return glyphs


def char_width(ch):
    """与 decl_text_width 一致：ASCII 8px，双字节 0x81..0xFE 16px。"""
    o = ord(ch)
    if o < 0x80:
        return 8
    if 0x81 <= o <= 0xFE:
        return 16
    return 0


def text_width(text, scale=1):
    """单行文本度量；scale 1/2，与 kernel decl_text_width 口径一致。"""
    if not text:
        return 0
    w = 0
    for ch in text:
        w += char_width(ch)
    return w * (scale or 1)


# ---------------------------------------------------------------------------
# 树遍历 / 几何工具
# ---------------------------------------------------------------------------


def iter_nodes(node):
    """先序遍历生成器。"""
    yield node
    for child in node.get("children") or []:
        yield from iter_nodes(child)


def parent_map(root):
    """返回 {id: parent_id}；根 parent=0。"""
    out = {}

    def walk(n, pid):
        out[n["id"]] = pid
        for c in n.get("children") or []:
            walk(c, n["id"])

    walk(root, 0)
    return out


def content_rect(node):
    """父内容矩形 = 节点 rect 去掉 padding（schema flush_rule 口径）。"""
    r = node["rect"]
    pad = node.get("padding", 0)
    if isinstance(pad, int):
        pt = pr = pb = pl = pad
    else:
        pt = pad.get("t", 0); pr = pad.get("r", 0)
        pb = pad.get("b", 0); pl = pad.get("l", 0)
    return {"x": r["x"] + pl, "y": r["y"] + pt,
            "w": r["w"] - pl - pr, "h": r["h"] - pt - pb}


def touch_sides(child, parent):
    """schema :: flush_rule：子块边与父内容矩形边完全相等即贴边。

    返回 [L, R, T, B] 子集；坐标按 8pt 量化后比较（这里 rect 已是量化值）。
    """
    cr = content_rect(parent)
    c = child["rect"]
    sides = []
    if c["x"] == cr["x"]:
        sides.append("L")
    if c["x"] + c["w"] == cr["x"] + cr["w"]:
        sides.append("R")
    if c["y"] == cr["y"]:
        sides.append("T")
    if c["y"] + c["h"] == cr["y"] + cr["h"]:
        sides.append("B")
    return sides


# 贴边侧 → 受影响的角（radius 归零的角）
SIDE_CORNERS = {
    "L": ["tl", "bl"],
    "R": ["tr", "br"],
    "T": ["tl", "tr"],
    "B": ["bl", "br"],
}


def snap8(v):
    """8pt 网格归整：向上取整到 8（0 保持 0）。"""
    v = int(v)
    if v <= 0:
        return 0
    return ((v + 7) // 8) * 8
