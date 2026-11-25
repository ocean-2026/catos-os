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
    "popover": 15, "icon": 16,
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
# cap2 能力扩展（规范 v0.3）：HTML 属性（《HTML 原型开发规范 v3》命名权威）
# ↔ 内核冻结字段（block.h / tokens.h 冻结表，禁改）唯一映射表。
#
# 纪律：HTML 属性名以 v3 清单为准，内核字段名以本表冻结值为准；两者映射
# 唯一，转换器 / 校验器 / schema.json / 规范 md 四处必须逐字一致（A-9-05..10）。
# 内核消费点符号随 feat/blk-cap2 线落地；基线源码未含这些消费者，本表字段名
# 已冻结，文档只照抄、不在工具线侧改名。
# ---------------------------------------------------------------------------

# 枚举映射：v3 属性值 → 内核 u8/u16 取值
CAP2_ALPHA_MODE = {"ordered16": 0, "none": 1}

CAP2_ELEVATION = {"flat": 0, "raised": 1, "hover": 2, "float": 3,
                  "overlay": 4, "modal": 5}

CAP2_EASE = {"none": 0, "linear": 1, "in": 2, "out": 3, "inout": 4,
             "out-back": 5, "in-back": 6, "step": 7}

CAP2_FONT_SIZE = {"8": 1, "caption": 1,
                   "16": 2, "body": 2,
                   "24": 3, "title": 3,
                   "32": 4, "display": 4}

CAP2_ELLIPSIS = {"none": 0, "char": 1, "word": 2}

CAP2_TINT = {"fg": 1, "bg": 2, "accent": 3, "border": 4}

CAP2_SCROLLBAR = {"auto": 0, "always": 1, "never": 2}

CAP2_SCROLL_AXIS = {"x": 1, "y": 2, "both": 3}

# data-anim 允许时长档（ms）；0 = 无动画（缺省）
CAP2_ANIM_MS = [0, 48, 96, 144, 192, 288]

# data-anim-props 位掩码：bit0 x / bit1 y / bit2 w / bit3 h /
# bit4 alpha / bit5 elevation
CAP2_ANIM_PROPS = {"x": 0, "y": 1, "w": 2, "h": 3,
                   "alpha": 4, "elevation": 5}

# data-prefer 位掩码：up1 / down2 / left4 / right8（可组合）
CAP2_POP_PREFER = {"up": 1, "down": 2, "left": 4, "right": 8}

# data-border-sides 位掩码：bit0 l / bit1 r / bit2 t / bit3 b；省略 = 0 = 全边
CAP2_BORDER_SIDES = {"l": 0, "r": 1, "t": 2, "b": 3}

# data-radius 命名哨兵：pill=128 / auto=255 / inherit=0（不显式，等价省略）
CAP2_RADIUS_SENTINEL = {"pill": 128, "auto": 255, "inherit": 0}

# data-wrap：0→0；1..6→同值；auto→255
CAP2_WRAP_AUTO = 255

# 逐角半径合法值域：0..63 px，外加哨兵 128 / 255；inherit 落 0。
CAP2_RADIUS_MAX_PX = 63

# cap2 冻结映射总账（审计 A-9-05..10 逐行核对 md / schema / 转换器三处一致）。
# field = 内核冻结字段名；default = 缺省（未声明）取值；rule = 诊断规则 ID；
# consume = 内核侧消费点（冻结文档口径，基线符号待 cap2 线回填）；
# assert = 内核侧验收断言 ID。
CAP2_FIELDS = [
    {"attr": "data-alpha", "field": "alpha", "ctype": "u8", "default": 0,
     "rule": "M-CAP2-01",
     "consume": "渲染包 blk_alpha_of / blk_cov",
     "assert": "cap2-alpha64/128/192"},
    {"attr": "data-alpha-mode", "field": "alpha_mode", "ctype": "u8",
     "default": 0, "rule": "M-CAP2-02",
     "consume": "渲染包 present 量化",
     "assert": "cap2-alpha64/128/192"},
    {"attr": "data-elevation", "field": "elevation", "ctype": "u8",
     "default": 0, "rule": "M-CAP2-03",
     "consume": "渲染包 9-slice 阴影",
     "assert": "cap2-shadow-slices"},
    {"attr": "data-radius", "field": "corner_tl/tr/br/bl", "ctype": "u8×4",
     "default": 0, "rule": "M-CAP2-04",
     "consume": "渲染包逐角掩码",
     "assert": "cap2-corner-per/pill/concentric"},
    {"attr": "data-radius-tl", "field": "corner_tl", "ctype": "u8",
     "default": 0, "rule": "M-CAP2-05",
     "consume": "渲染包逐角掩码", "assert": "cap2-corner-per"},
    {"attr": "data-radius-tr", "field": "corner_tr", "ctype": "u8",
     "default": 0, "rule": "M-CAP2-05",
     "consume": "渲染包逐角掩码", "assert": "cap2-corner-per"},
    {"attr": "data-radius-br", "field": "corner_br", "ctype": "u8",
     "default": 0, "rule": "M-CAP2-05",
     "consume": "渲染包逐角掩码", "assert": "cap2-corner-per"},
    {"attr": "data-radius-bl", "field": "corner_bl", "ctype": "u8",
     "default": 0, "rule": "M-CAP2-05",
     "consume": "渲染包逐角掩码", "assert": "cap2-corner-per"},
    {"attr": "data-border-sides", "field": "border_sides", "ctype": "u8",
     "default": 0, "rule": "M-CAP2-06",
     "consume": "渲染包 body_border",
     "assert": "cap2-border-sides"},
    {"attr": "data-wrap", "field": "wrap_lines", "ctype": "u8",
     "default": 0, "rule": "M-CAP2-07",
     "consume": "文本包 txt_wrap_lines",
     "assert": "cap2-wrap1/wrap2/wrapauto"},
    {"attr": "data-ellipsis", "field": "ellipsis", "ctype": "u8",
     "default": 0, "rule": "M-CAP2-08",
     "consume": "文本包", "assert": "cap2-ellipsis-char/word"},
    {"attr": "data-font", "field": "font_size", "ctype": "u8",
     "default": 0, "rule": "M-CAP2-09",
     "consume": "文本包 txt_font_h",
     "assert": "cap2-font8/16/24/32"},
    {"attr": "data-bold", "field": "font_bold", "ctype": "u8",
     "default": 0, "rule": "M-CAP2-10",
     "consume": "文本包", "assert": "cap2-bold"},
    {"attr": "data-tabular", "field": "font_tabular", "ctype": "u8",
     "default": 0, "rule": "M-CAP2-11",
     "consume": "文本包等宽数字", "assert": "cap2-tabular"},
    {"attr": "data-line-h", "field": "line_h", "ctype": "u8",
     "default": 0, "rule": "M-CAP2-12",
     "consume": "文本包", "assert": "cap2-wrap* 行高"},
    {"attr": "data-anim", "field": "anim_ms", "ctype": "u16",
     "default": 0, "rule": "M-CAP2-13",
     "consume": "动画包预生成帧表",
     "assert": "cap2-anim-outback"},
    {"attr": "data-ease", "field": "anim_ease", "ctype": "u8",
     "default": 0, "rule": "M-CAP2-14",
     "consume": "动画包", "assert": "cap2-anim-outback"},
    {"attr": "data-anim-props", "field": "anim_mask", "ctype": "u8",
     "default": 0, "rule": "M-CAP2-15",
     "consume": "动画包", "assert": "cap2-anim-interrupt"},
    {"attr": "data-anchor", "field": "anchor_id", "ctype": "u32",
     "default": 0, "rule": "M-CAP2-16",
     "consume": "浮层包 pop_layout", "assert": "cap2-pop-*"},
    {"attr": "data-prefer", "field": "pop_prefer", "ctype": "u8",
     "default": 0, "rule": "M-CAP2-17",
     "consume": "浮层包", "assert": "cap2-pop-down/up/left/right"},
    {"attr": "data-max-h", "field": "pop_max_h", "ctype": "u16",
     "default": 0, "rule": "M-CAP2-18",
     "consume": "浮层包内部滚动", "assert": "cap2-pop-scroll"},
    {"attr": "data-icon", "field": "icon", "ctype": "u8",
     "default": 0, "rule": "M-CAP2-19",
     "consume": "icon_draw", "assert": "cap2-icon-tint"},
    {"attr": "data-tint", "field": "tint_role", "ctype": "u8",
     "default": 0, "rule": "M-CAP2-20",
     "consume": "icon_draw", "assert": "cap2-icon-tint"},
    {"attr": "data-scrollbar", "field": "scrollbar", "ctype": "u8",
     "default": 0, "rule": "M-CAP2-21",
     "consume": "渲染包 sb_visible/sb_paint",
     "assert": "cap2-sb-paint"},
    {"attr": "data-scroll", "field": "scroll_axis", "ctype": "u8",
     "default": 0, "rule": "M-CAP2-22",
     "consume": "滚动条", "assert": "cap2-sb-wheel"},
    {"attr": "data-theme", "field": "hc_force", "ctype": "u8",
     "default": 0, "rule": "M-CAP2-23",
     "consume": "令牌/渲染", "assert": "cap2-hc-override"},
]

# cap2 自定义标签：标签 → kind 枚举值（与 kind_enum 同步）
CAP2_TAGS = {"c-popover": "popover", "c-icon": "icon"}

# ---------------------------------------------------------------------------
# cap3 命名增量（v0.3，已评审定稿，append-only 冻结）。
# 形态（corner_shape / corner_n）是整块级别，与 cap2 逐角
# corner_tl/tr/br/bl 正交：形态整块统一，半径仍可逐角。
# ---------------------------------------------------------------------------

# data-radius-shape：arc=0（默认）/ squircle=1；g2 为 squircle 别名。
# 禁止用 data-radius="g2 16" 这类关键字塞数值属性（R-7.5-01）。
CAP3_RADIUS_SHAPE = {"arc": 0, "squircle": 1, "g2": 1}

# corner_n:u8，单位 0.1；0 = 默认 50（5.0）；有效区间 10..50（1.0..5.0），
# 越界钳到 50 并报 WARN_RADIUS_N_RANGE；非数值指数 → E_RADIUS_N_BAD。
CAP3_CORNER_N_MIN = 10
CAP3_CORNER_N_MAX = 50
CAP3_CORNER_N_DEFAULT = 0

# cap3 冻结映射总账（审计 A-9-05..10 同样覆盖）。
# CE 侧 ce_blk_rec2_t 只下发 corner_shape，不下发 corner_n。
CAP3_FIELDS = [
    {"attr": "data-radius-shape", "field": "corner_shape", "ctype": "u8",
     "default": 0, "rule": "E_RADIUS_SHAPE_BAD",
     "consume": "渲染包逐角掩码 / 形态采样",
     "assert": "cap2-corner-per; R-7.5-01/02/03"},
    {"attr": "data-squircle-n", "field": "corner_n", "ctype": "u8",
     "default": 0, "rule": "E_RADIUS_N_BAD",
     "consume": "渲染包 squircle 指数（仅 SQUIRCLE 有效）",
     "assert": "R-7.5-02/03"},
]

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
