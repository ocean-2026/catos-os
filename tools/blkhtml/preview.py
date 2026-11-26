#!/usr/bin/env python3
"""preview -- .blk.json → 单文件 HTML / 纯净 PNG（app|calm|design 三口径）。

用法：
    python3 preview.py page.blk.json [--html out.html] [--png out.png]
                       [--mode app|calm|design] [--theme dark|light|hc]
                       [--font-src PATH]

口径（FROZEN.md §3）：
  app    直角 + 1px 描边 + 无阴影（内核 style=app 口径）。
  calm   尺寸阶梯 8/12/16/24 超椭圆 n≈5 圆角 + paint_halo 层决定双层阴影。
  design 逐角 radius + shadow.sides，并执行 flush_rule（目标态，内核暂不具备）。

文字逐像素解析 font_ascii.inc 绘制；度量与 decl_text_width 一致
（ASCII 8px、双字节 16px，scale 1/2）。PNG 由 zlib+struct 手写，不装库。
"""

import argparse
import json
import os
import struct
import sys
import zlib

import blkcommon as bc

CALM_RADII = [8, 12, 16, 24]


# ---------------------------------------------------------------------------
# 颜色解析
# ---------------------------------------------------------------------------


def resolve(node, role_key, theme, default=None):
    val = node.get(role_key)
    if val is None:
        return default
    if val in bc.ROLE_NAMES:
        return bc.palette(theme)[val]
    if val.startswith("#"):
        return val[1:]
    return val


# ---------------------------------------------------------------------------
# 圆角掩码（复刻 paint.c calm_masks：超椭圆 n≈5）
# ---------------------------------------------------------------------------


def _mask(r):
    mask = [[0] * r for _ in range(r)]
    r5 = r ** 5
    for dy in range(r):
        for dx in range(r):
            u = r - dx
            v = r - dy
            mask[dy][dx] = 1 if u ** 5 + v ** 5 <= r5 else 0
    return mask


def _inset_left(mask, row, r):
    for dx in range(r):
        if mask[row][dx]:
            return dx
    return r


def calm_r_index(w, h):
    s = w if w < h else h
    if s >= 128:
        return 3
    if s >= 64:
        return 2
    if s >= 32:
        return 1
    return 0


# ---------------------------------------------------------------------------
# cap2 / cap3 离线渲染纯函数（可复核：确定性，无浏览器特效近似）
# ---------------------------------------------------------------------------

# 4×4 Bayer 有序抖动阈值表（16 档，0..15），与内核 ordered16 同算法。
BAYER4 = [
    [0, 8, 2, 10],
    [12, 4, 14, 6],
    [3, 11, 1, 9],
    [15, 7, 13, 5],
]

# elevation 0..5 阴影档：(黑千分比, 右下偏移 x, y, 外扩 px)。
ELEV_SHADOW = [
    (0, 0, 0, 0), (60, 1, 1, 1), (100, 1, 2, 1), (140, 2, 3, 2),
    (180, 2, 4, 2), (220, 3, 5, 3),
]

# font_size 档：(advance px/ASCII, 字高 px)；0 = 令牌默认 body 档。
FONT_METRICS = {0: (8, 16), 1: (8, 8), 2: (8, 16), 3: (12, 24),
                 4: (16, 32)}

# 七缓动（anim_ease 1..7；0=none 恒 0）。t ∈ [0,1]。
def ease_func(ease_id, t):
    t = 0.0 if t < 0 else 1.0 if t > 1 else t
    if ease_id == 1:    # linear
        return t
    if ease_id == 2:    # in
        return t * t
    if ease_id == 3:    # out
        return 1 - (1 - t) ** 2
    if ease_id == 4:    # inout
        return 3 * t * t - 2 * t * t * t
    if ease_id == 5:    # out-back
        c1 = 1.70158
        c3 = c1 + 1
        return 1 + c3 * (t - 1) ** 3 + c1 * (t - 1) ** 2
    if ease_id == 6:    # in-back
        c1 = 1.70158
        c3 = c1 + 1
        return c3 * t ** 3 - c1 * t ** 2
    if ease_id == 7:    # step
        return 0.0 if t < 1 else 1.0
    return 0.0


def anim_progress(anim_ms, ease_id, tick_ms):
    """按 16ms tick 预生成偏移表：返回 [0,1] 进度。anim_ms=0 → 0。"""
    if not anim_ms:
        return 0.0
    t = tick_ms / anim_ms
    return ease_func(ease_id or 1, t)


def dither_on(x, y, alpha, alpha_mode):
    """ordered16：Bayer 阈值抖动；none：最近档不抖动。alpha 0..255。"""
    if alpha <= 0:
        return False
    if alpha >= 255:
        return True
    coverage = alpha / 255.0
    if alpha_mode == 1:  # none
        return coverage >= 0.5
    thresh = BAYER4[y & 3][x & 3] / 16.0
    return coverage > thresh


def wrap_text(text, width, advance, wrap, ellipsis):
    """多行排版。wrap: 0 单行 / 1..6 行数 / 255 auto；ellipsis 0 none/1 char/2 word。

    返回行列表；超出行数时末行按 char/word 补 ``...``。纯确定性度量。
    """
    if not text:
        return []
    max_lines = 6 if wrap == 255 else (1 if wrap == 0 else wrap)
    if ellipsis == 2:  # word 折行
        words = text.split(" ")
        lines, cur = [], ""
        for w in words:
            cand = w if not cur else cur + " " + w
            if advance_len(cand, advance) <= width:
                cur = cand
            else:
                if cur:
                    lines.append(cur)
                cur = w
        if cur:
            lines.append(cur)
    else:  # char 折行（默认）
        lines, cur = [], ""
        for ch in text:
            if advance_len(cur + ch, advance) <= width:
                cur += ch
            else:
                lines.append(cur)
                cur = ch
        if cur:
            lines.append(cur)
    if len(lines) <= max_lines:
        return lines
    kept = lines[:max_lines]
    last = kept[-1]
    if ellipsis != 0:
        while last and advance_len(last + "...", advance) > width:
            last = last[:-1]
        last = last + "..."
        kept[-1] = last
    return kept


def advance_len(s, advance):
    return len(s) * advance


def corner_radius(corner, node, w, h):
    """解析逐角半径：0..63 / 128 pill=短边/2 / 255 auto=阶梯 / 0 inherit。"""
    rad = node.get("radius") or {}
    v = rad.get(corner, 0)
    if v == 128:
        return min(w, h) // 2
    if v == 255:
        return CALM_RADII[calm_r_index(w, h)]
    return v


def shape_coverage(px, py, r, w, h, corner, shape, n):
    """4×4 子采样面积法：返回该像素在角掩码内的覆盖率 0..1。

    arc：四分之一圆；squircle：超椭圆 |u/r|^n+|v/r|^n ≤ 1。
    """
    if r <= 0:
        return 1.0
    ox = 0 if corner in ("tl", "bl") else w - r
    oy = 0 if corner in ("tl", "tr") else h - r
    inside = 0
    for sy in range(4):
        for sx in range(4):
            x = px + (sx + 0.5) / 4 - ox
            y = py + (sy + 0.5) / 4 - oy
            if corner in ("tl", "bl"):
                x = r - x
            else:
                x = r - x
            if corner in ("tl", "tr"):
                y = r - y
            else:
                y = r - y
            if shape == 1:  # squircle
                if (abs(x / r) ** n + abs(y / r) ** n) <= 1:
                    inside += 1
            else:           # arc 圆
                if x * x + y * y <= r * r:
                    inside += 1
    return inside / 16.0


# 内置单色图标（16×16，16 行 × 16 位 MSB 左），≥8 个。
ICON_ROWS = {
    "home": ["0000000110000000", "0000001111000000", "0000111111100000",
             "0001111111110000", "0011111111111000", "0111111111111100",
             "0111100110011100", "0111100110011100", "0111100110011100",
             "0111100110011100", "0111100110011100", "0000000000000000",
             "0000000000000000", "0000000000000000", "0000000000000000",
             "0000000000000000"],
    "gear": ["0000111111000000", "0001100001100000", "0011011110110000",
             "0110111111011000", "0111101101111000", "1111011110111100",
             "1111011110111100", "0111101101111000", "0110111111011000",
             "0011011110110000", "0001100001100000", "0000111111000000",
             "0000000000000000", "0000000000000000", "0000000000000000",
             "0000000000000000"],
    "bell": ["0000011110000000", "0000111111000000", "0001111111100000",
             "0001111111100000", "0001111111100000", "0001111111100000",
             "0001111111100000", "0001111111100000", "0001111111100000",
             "0000111111000000", "0000011110000000", "0000001100000000",
             "0000011110000000", "0000000000000000", "0000000000000000",
             "0000000000000000"],
    "clock": ["0000111111000000", "0001100001100000", "0011000000110000",
             "0110001100011000", "0110011110011000", "1100011110001100",
             "1100011110001100", "1100011110001100", "0110011110011000",
             "0110001100011000", "0011000000110000", "0001100001100000",
             "0000111111000000", "0000000000000000", "0000000000000000",
             "0000000000000000"],
    "close": ["1100000000000011", "1110000000000110", "0111000000011100",
              "0011100000111000", "0001110001110000", "0000111011100000",
              "0000011111000000", "0000001110000000", "0000011111000000",
              "0000111011100000", "0001110001110000", "0011100000111000",
              "0111000000011100", "1110000000000110", "1100000000000011",
              "0000000000000000"],
    "check": ["0000000000011000", "0000000000111000", "0000000001110000",
              "0000000011110000", "0000000111100000", "0000001111000000",
              "0000011110000000", "0000111100000000", "0001111000000000",
              "0011110000000000", "0111100000000000", "1111000000000000",
              "0000000000000000", "0000000000000000", "0000000000000000",
              "0000000000000000"],
    "search": ["0001111100000000", "0011000110000000", "0110000011000000",
               "0110000001100000", "1100000001100000", "1100000000110000",
               "1100000000110000", "0110000001100000", "0110000011000000",
               "0011000110000000", "0001111100011000", "0000000001111000",
               "0000000000011100", "0000000000000000", "0000000000000000",
               "0000000000000000"],
    "arrow": ["0000000110000000", "0000001110000000", "0000111110000000",
              "0001111110000000", "0111111111111000", "1111111111111100",
              "0111111111111000", "0001111110000000", "0000111110000000",
              "0000001110000000", "0000000110000000", "0000000000000000",
              "0000000000000000", "0000000000000000", "0000000000000000",
              "0000000000000000"],
}

ICON_NAMES = tuple(ICON_ROWS.keys())


def icon_pixels(name, size=16):
    """返回图标 (x,y) 集合；name 缺省/未知回退 home。size 16 或 32（2× 倍点）。"""
    rows = ICON_ROWS.get(name) or ICON_ROWS["home"]
    pts = set()
    scale = 2 if size >= 32 else 1
    for ry, row in enumerate(rows):
        for bit, ch in enumerate(row):
            if ch == "1":
                for sy in range(scale):
                    for sx in range(scale):
                        pts.add((bit * scale + sx, ry * scale + sy))
    return pts


def pop_layout(anchor, pop_w, pop_h, prefer, max_h, bounds):
    """浮层定位：锚定 → 收缩 → 8px 安全边距钳制 → 四向翻转。

    anchor: 锚块 rect；bounds: 根 rect。prefer 位 up1/down2/left4/right8。
    返回 (rect, dir_name)。超 max_h 时高度收缩到 max_h（内部滚动由 scrollbar 表达）。
    """
    if max_h and pop_h > max_h:
        pop_h = max_h
    ax, ay, aw, ah = anchor["x"], anchor["y"], anchor["w"], anchor["h"]
    bx, by, bw, bh = bounds["x"], bounds["y"], bounds["w"], bounds["h"]
    order = []
    if prefer & 2:
        order.append("down")
    if prefer & 1:
        order.append("up")
    if prefer & 8:
        order.append("right")
    if prefer & 4:
        order.append("left")
    if not order:
        order = ["down", "right", "up", "left"]
    margin = 8

    def clamp(rx, ry, rw, rh):
        rx = max(bx + margin, min(rx, bx + bw - rw - margin))
        ry = max(by + margin, min(ry, by + bh - rh - margin))
        return {"x": rx, "y": ry, "w": rw, "h": rh}

    candidates = {
        "down": (ax, ay + ah + margin, pop_w, pop_h),
        "up": (ax, ay - pop_h - margin, pop_w, pop_h),
        "right": (ax + aw + margin, ay, pop_w, pop_h),
        "left": (ax - pop_w - margin, ay, pop_w, pop_h),
    }
    for direction in order:
        rx, ry, rw, rh = candidates[direction]
        if (bx + margin <= rx <= bx + bw - rw - margin and
                by + margin <= ry <= by + bh - rh - margin):
            return {"x": rx, "y": ry, "w": rw, "h": rh}, direction
    # 全部越界：按首选方向钳制进边界
    d = order[0]
    rx, ry, rw, rh = candidates[d]
    return clamp(rx, ry, rw, rh), d


def scrollbar_thumb(viewport, content, thickness=6):
    """scrollbar thumb：比例 = 视口/内容；内容 ≤ 视口返回 None。"""
    if content <= viewport:
        return None
    ratio = viewport / content
    thumb = max(thickness, int(round(viewport * ratio)))
    return {"thickness": thickness, "thumb": thumb,
            "ratio": round(ratio, 4)}


# ---------------------------------------------------------------------------
# 像素缓冲（RGB）
# ---------------------------------------------------------------------------


class PixBuf:
    def __init__(self, w, h, bg):
        self.w = w
        self.h = h
        self.bg = bg
        self.rows = [bytearray(bytes.fromhex(bg) * w) for _ in range(h)]

    def fill_rect(self, x, y, w, h, color):
        r, g, b = bytes.fromhex(color)
        for row in range(max(0, y), min(self.h, y + h)):
            off = max(0, x) * 3
            end = min(self.w, x + w) * 3
            self.rows[row][off:end] = bytearray([r, g, b]) * ((end - off) // 3)

    def border(self, x, y, w, h, color, bw=1):
        for _ in range(bw):
            self.fill_rect(x, y, w, 1, color)
            self.fill_rect(x, y + h - 1, w, 1, color)
            self.fill_rect(x, y, 1, h, color)
            self.fill_rect(x + w - 1, y, 1, h, color)
            x += 1; y += 1; w -= 2; h -= 2

    def _row_span(self, row, x, w, color):
        if row < 0 or row >= self.h:
            return
        x = max(0, x)
        w = min(self.w - x, w)
        if w <= 0:
            return
        r, g, b = bytes.fromhex(color)
        off = x * 3
        self.rows[row][off:off + w * 3] = bytearray([r, g, b]) * w

    def blend_pixel(self, x, y, color, alpha):
        """按 alpha 0..255 与底色混合（无抖动）；越界忽略。"""
        if not (0 <= x < self.w and 0 <= y < self.h):
            return
        fr, fg, fb = bytes.fromhex(color)
        bg = self.rows[y]
        off = x * 3
        a = alpha / 255.0
        bg[off] = round(fr * a + bg[off] * (1 - a))
        bg[off + 1] = round(fg * a + bg[off + 1] * (1 - a))
        bg[off + 2] = round(fb * a + bg[off + 2] * (1 - a))

    def fill_alpha(self, x, y, w, h, color, alpha, alpha_mode=0):
        """带透明填充：ordered16 Bayer 抖动；none 最近档。alpha 0..255。"""
        if alpha is None or alpha >= 255:
            self.fill_rect(x, y, w, h, color)
            return
        if alpha <= 0:
            return
        r, g, b = bytes.fromhex(color)
        for row in range(max(0, y), min(self.h, y + h)):
            for col in range(max(0, x), min(self.w, x + w)):
                if dither_on(col, row, alpha, alpha_mode):
                    off = col * 3
                    self.rows[row][off:off + 3] = bytearray([r, g, b])

    def fill_shape(self, x, y, w, h, corners, color, shape=0, n=5.0):
        """形态感知填充：arc 圆角 / squircle 超椭圆，4×4 面积子采样。"""
        x = max(0, x)
        y = max(0, y)
        w = min(self.w - x, w)
        h = min(self.h - y, h)
        if w <= 0 or h <= 0:
            return
        r, g, b = bytes.fromhex(color)
        rad = {c: corners.get(c, 0) for c in ("tl", "tr", "br", "bl")}
        for row in range(h):
            for col in range(w):
                cov = 1.0
                for c, rr in rad.items():
                    if rr > 0:
                        cov = min(cov, shape_coverage(col, row, rr, w, h,
                                                     c, shape, n))
                if cov >= 8 / 16:
                    off = (x + col) * 3
                    self.rows[y + row][off:off + 3] = bytearray([r, g, b])

    def draw_icon(self, x, y, name, color, size=16):
        for px, py in icon_pixels(name, size):
            self.blend_pixel(x + px, y + py, color, 255)

    def fill_round(self, x, y, w, h, radius, color, corners=None):
        """圆角填充。corners: {'tl':r,'tr':r,'br':r,'bl':r}（design 逐角）。"""
        if radius <= 0 or w <= 0 or h <= 0:
            self.fill_rect(x, y, w, h, color)
            return
        if corners is None:
            corners = {c: radius for c in ("tl", "tr", "br", "bl")}
        # 各角掩码缓存
        masks = {}
        for c, r in corners.items():
            masks[c] = _mask(r) if r > 0 else None
        for row in range(h):
            inset_l = 0
            inset_r = 0
            if row < corners["tl"] and masks["tl"]:
                inset_l = _inset_left(masks["tl"], row, corners["tl"])
            if row >= h - corners["bl"] and masks["bl"]:
                ry = corners["bl"] - 1 - (h - 1 - row)
                inset_l = max(inset_l, _inset_left(masks["bl"], ry, corners["bl"]))
            if row < corners["tr"] and masks["tr"]:
                inset_r = _inset_left(masks["tr"], row, corners["tr"])
            if row >= h - corners["br"] and masks["br"]:
                ry = corners["br"] - 1 - (h - 1 - row)
                inset_r = max(inset_r, _inset_left(masks["br"], ry, corners["br"]))
            self._row_span(y + row, x + inset_l, w - inset_l - inset_r, color)


# ---------------------------------------------------------------------------
# 文本绘制（font_ascii.inc 字模，MSB 左）
# ---------------------------------------------------------------------------


def fit_text(text, width, scale, font):
    """按 decl_text_width 度量，超宽 ellipsis 截断。返回可放下的字符串。"""
    if bc.text_width(text, scale) <= width:
        return text
    keep = ""
    for ch in text:
        if bc.text_width(keep + ch + "...", scale) <= width:
            keep += ch
        else:
            break
    return keep + "..." if keep else "..."


def _draw_glyph(buf, font, ch, ox, oy, advance, cell_h, bold, col):
    """画一个字形；返回步进宽度。8×16 字模按 cell_h 纵向映射。"""
    glyph = font.get(ord(ch))
    if not glyph:
        return advance
    rows = len(glyph)
    for ry in range(cell_h):
        gy = min(rows - 1, int(ry * rows / cell_h))
        byte = glyph[gy]
        for bit in range(8):
            if byte & (0x80 >> bit):
                px = ox + int(bit * advance / 8)
                buf.blend_pixel(px, oy + ry, col, 255)
                if bold:
                    buf.blend_pixel(px + 1, oy + ry, col, 255)
    return advance


def draw_text(buf, node, theme, font):
    text = node.get("text")
    if not text:
        return
    r = node["rect"]
    size = node.get("font_size", 0)
    advance, cell_h = FONT_METRICS[size]
    wrap = node.get("wrap_lines", 0)
    ell = node.get("ellipsis", 0)
    width = r["w"] - 8
    lines = wrap_text(text, width, advance, wrap, ell)
    line_h = node.get("line_h") or 24
    fg = resolve(node, "fg", theme, bc.palette(theme)["fg"])
    bold = node.get("font_bold", 0)
    oy0 = r["y"] + max(0, (r["h"] - line_h * len(lines)) // 2)
    for li, line in enumerate(lines):
        ox = r["x"] + 4
        oy = oy0 + li * line_h
        for ch in line:
            ox += _draw_glyph(buf, font, ch, ox, oy, advance, cell_h,
                              bold, fg)


# ---------------------------------------------------------------------------
# 场景渲染
# ---------------------------------------------------------------------------


def _stroke_color(node, theme):
    """描边色：角色名 → token_palette；#RRGGBB → 原值；缺失回退 border 角色。"""
    stroke = node.get("stroke") or {}
    col = stroke.get("color")
    if not col:
        return bc.palette(theme)["border"]
    if col in bc.ROLE_NAMES:
        return bc.palette(theme)[col]
    if col.startswith("#"):
        return col[1:]
    return col


def _calm_eligible(node, theme):
    if node["kind"] == "root" or node.get("bleed"):
        return False
    if node.get("layer") in ("chrome", "bg"):
        return False
    if theme == "hc":
        return False
    return True


def _tint_color(node, theme):
    """data-tint → 角色色；tint_role 1 fg / 2 bg / 3 accent / 4 border。"""
    role = node.get("tint_role", 0)
    pal = bc.palette(theme)
    return {1: pal["fg"], 2: pal["bg"], 3: pal["accent"],
            4: pal["border"]}.get(role, pal["fg"])


def _draw_elevation_shadow(buf, node, parent, theme):
    """elevation 0..5 右下光源 9-slice 阴影（块体外）；HC 用 1px 边框替代。"""
    elev = node.get("elevation", 0)
    if not elev:
        return
    r = node["rect"]
    if theme == "hc":
        buf.border(r["x"], r["y"], r["w"], r["h"],
                   bc.palette(theme)["border"], 1)
        return
    permil, ox, oy, expand = ELEV_SHADOW[elev]
    under = resolve(parent, "fill", theme) if parent else \
        bc.palette(theme)["bg"]
    if under is None:
        under = bc.palette(theme)["bg"]
    col = bc.mix_black(under, permil)
    buf.border(r["x"] - expand + ox, r["y"] - expand + oy,
               r["w"] + 2 * expand, r["h"] + 2 * expand, col, 1)


def _draw_scrollbar(buf, node, theme):
    """scrollbar auto/always/never：track + 6px thumb，比例=视口/内容。"""
    sb = node.get("scrollbar", 0)
    if sb == 2:
        return
    r = node["rect"]
    axis = node.get("scroll_axis", 0)
    children = node.get("children") or []
    if not children:
        return
    content = max((c["rect"]["y"] + c["rect"]["h"] for c in children),
                  default=r["h"]) - r["y"]
    viewport = r["h"] if axis != 1 else r["w"]
    meta = scrollbar_thumb(viewport, content)
    if meta is None:
        if sb != 1:
            return
        thumb = viewport
    else:
        thumb = meta["thumb"]
    pal = bc.palette(theme)
    track = pal["line"]
    col = pal["bar"]
    if axis == 1:  # x 轴
        tx, ty = r["x"], r["y"] + r["h"] - 6
        buf.fill_rect(tx, ty, r["w"], 6, track)
        buf.fill_rect(tx, ty, thumb, 6, col)
    else:          # y 轴（含 both）
        tx, ty = r["x"] + r["w"] - 6, r["y"]
        buf.fill_rect(tx, ty, 6, r["h"], track)
        buf.fill_rect(tx, ty, 6, thumb, col)


def render_png(tree, mode, theme, font_src=None, frame_tick=None):
    root = tree["root"]
    fr = root["rect"]
    buf = PixBuf(fr["w"], fr["h"], bc.palette(theme)["bg"])
    font = bc.load_font(font_src)
    by_id = {n["id"]: n for n in bc.iter_nodes(root)}

    def walk(node, parent, eff_alpha):
        r, anim_alpha = _anim_node(node, frame_tick)
        # 出界裁剪：动画起点/翻页偏移可能落在根边界外，禁止负坐标写入。
        r = {
            "x": max(0, min(fr["w"], r["x"])),
            "y": max(0, min(fr["h"], r["y"])),
            "w": max(0, min(fr["w"] - r["x"], r["w"])),
            "h": max(0, min(fr["h"] - r["y"], r["h"])),
        }
        if node["kind"] == "popover":
            anchor = by_id.get(node.get("anchor_id", 0))
            if anchor is not None:
                r, _dir = pop_layout(anchor["rect"], r["w"], r["h"],
                                     node.get("pop_prefer", 0),
                                     node.get("pop_max_h", 0), fr)
                node["_preview_rect"] = r
        kind = node["kind"]
        fill = resolve(node, "fill", theme)
        # cap2 alpha：0=未声明(≡255)；子树乘法继承 父×子/255。
        alpha = node.get("alpha", 0) if anim_alpha is None else anim_alpha
        own = 255 if alpha == 0 else alpha
        eff = own * eff_alpha // 255
        alpha_mode = node.get("alpha_mode", 0)

        if kind == "icon":
            idx = node.get("icon", 0) % len(ICON_NAMES)
            buf.draw_icon(r["x"], r["y"], ICON_NAMES[idx],
                          _tint_color(node, theme),
                          32 if r["w"] >= 32 else 16)
        elif kind in ("column", "row", "stack", "free", "root", "rect",
                      "button", "input", "panel", "popover"):
            _draw_elevation_shadow(buf, node, parent, theme)
            if fill:
                shape = node.get("corner_shape", 0)
                n = (node.get("corner_n", 0) or 50) / 10.0
                corners = {c: corner_radius(c, node, r["w"], r["h"])
                           for c in ("tl", "tr", "br", "bl")}
                if shape in (0, 1) and any(corners.values()):
                    buf.fill_shape(r["x"], r["y"], r["w"], r["h"],
                                   corners, fill, shape, n)
                elif mode == "calm" and _calm_eligible(node, theme):
                    ri = calm_r_index(r["w"], r["h"])
                    buf.fill_round(r["x"], r["y"], r["w"], r["h"],
                                   CALM_RADII[ri], fill)
                elif mode == "design" and node.get("radius"):
                    buf.fill_round(r["x"], r["y"], r["w"], r["h"], 0, fill,
                                   node["radius"])
                elif eff < 255:
                    buf.fill_alpha(r["x"], r["y"], r["w"], r["h"], fill,
                                   eff, alpha_mode)
                else:
                    buf.fill_rect(r["x"], r["y"], r["w"], r["h"], fill)

        _draw_scrollbar(buf, node, theme)

        # 描边
        stroke = node.get("stroke")
        sides = (stroke or {}).get("sides") or ["L", "R", "T", "B"]
        bw = (stroke or {}).get("w", 0)
        if mode == "app":
            bw = bw if bw else (1 if kind not in ("text", "image", "divider") else 0)
            if bw:
                buf.border(r["x"], r["y"], r["w"], r["h"],
                           _stroke_color(node, theme), bw)
        elif mode == "calm" and _calm_eligible(node, theme):
            ri = calm_r_index(r["w"], r["h"])
            buf.fill_round(r["x"], r["y"], r["w"], r["h"], CALM_RADII[ri],
                            _stroke_color(node, theme))
            inner = (r["x"] + bw, r["y"] + bw, r["w"] - 2 * bw, r["h"] - 2 * bw)
            if fill and inner[2] > 0 and inner[3] > 0:
                buf.fill_round(inner[0], inner[1], inner[2], inner[3],
                               CALM_RADII[ri], fill)
        elif mode == "design":
            if stroke and bw:
                col = _stroke_color(node, theme)
                # 逐边描边（贴边侧已在 html2blk/flush 中移除）
                if "L" in sides:
                    buf.fill_rect(r["x"], r["y"], 1, r["h"], col)
                if "R" in sides:
                    buf.fill_rect(r["x"] + r["w"] - 1, r["y"], 1, r["h"], col)
                if "T" in sides:
                    buf.fill_rect(r["x"], r["y"], r["w"], 1, col)
                if "B" in sides:
                    buf.fill_rect(r["x"], r["y"] + r["h"] - 1, r["w"], 1, col)

        # 阴影（calm 按 layer 双层；design 按 shadow.sides）
        if mode == "calm" and _calm_eligible(node, theme):
            under = resolve(parent, "fill", theme) if parent else \
                bc.palette(theme)["bg"]
            if under is None:
                under = bc.palette(theme)["bg"]
            contact = bc.mix_black(under, 180)
            buf.border(r["x"], r["y"] + 1, r["w"], r["h"], contact, 1)
            if node.get("layer") in ("overlay", "toast"):
                drop = bc.mix_black(under, 100)
                buf.border(r["x"], r["y"] + 3, r["w"], r["h"], drop, 1)
                buf.border(r["x"], r["y"] + 4, r["w"], r["h"], drop, 1)
        if mode == "design":
            shadow = node.get("shadow") or {}
            sides = shadow.get("sides") or []
            if sides:
                under = resolve(parent, "fill", theme) if parent else \
                    bc.palette(theme)["bg"]
                if under is None:
                    under = bc.palette(theme)["bg"]
                col = bc.mix_black(under, 180)
                if "T" in sides:
                    buf.fill_rect(r["x"], r["y"] - 1, r["w"], 1, col)
                if "B" in sides:
                    buf.fill_rect(r["x"], r["y"] + r["h"], r["w"], 1, col)
                if "L" in sides:
                    buf.fill_rect(r["x"] - 1, r["y"], 1, r["h"], col)
                if "R" in sides:
                    buf.fill_rect(r["x"] + r["w"], r["y"], 1, r["h"], col)

        draw_text(buf, node, theme, font)
        for c in node.get("children") or []:
            walk(c, node, eff)

    walk(root, None, 255)
    return buf


def _anim_node(node, tick):
    """按 anim_mask 与缓动进度求当前帧 rect/alpha；无动画原样返回。

    起点相对基础块：x/y/w/h 各退 16px，alpha 退到 30%；进度 t 由
    ``anim_progress`` 按 16ms tick 与 7 条缓动之一算出。仅预览离线推演，
    不改写产物块树。
    """
    ms = node.get("anim_ms", 0)
    if not ms or tick is None:
        return node["rect"], node.get("alpha", 0)
    t = anim_progress(ms, node.get("anim_ease", 1), tick)
    mask = node.get("anim_mask", 0)
    base = node["rect"]
    rx, ry, rw, rh = base["x"], base["y"], base["w"], base["h"]
    if mask & 1:
        rx = round(rx - 16 * (1 - t))
    if mask & 2:
        ry = round(ry - 16 * (1 - t))
    if mask & 4:
        rw = round(rw - 16 * (1 - t))
    if mask & 8:
        rh = round(rh - 16 * (1 - t))
    alpha = node.get("alpha", 0)
    if mask & 16 and alpha:
        alpha = round(alpha * (0.3 + 0.7 * t))
    return {"x": rx, "y": ry, "w": rw, "h": rh}, alpha


# ---------------------------------------------------------------------------
# PNG 编码（zlib + struct，无第三方库）
# ---------------------------------------------------------------------------


def _chunk(tag, data):
    return (struct.pack(">I", len(data)) + tag + data +
            struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))


def encode_png(width, height, rows):
    raw = bytearray()
    for row in rows:
        raw += b"\x00"
        raw += bytes(row)
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    png = (b"\x89PNG\r\n\x1a\n" + _chunk(b"IHDR", ihdr) +
           _chunk(b"IDAT", zlib.compress(bytes(raw), 9)) +
           _chunk(b"IEND", b""))
    return png


def write_png(path, buf):
    png = encode_png(buf.w, buf.h, buf.rows)
    with open(path, "wb") as f:
        f.write(png)
    return len(png)


# ---------------------------------------------------------------------------
# HTML 预览（单文件）
# ---------------------------------------------------------------------------


def render_html(tree, mode, theme):
    root = tree["root"]
    pal = bc.palette(theme)
    parts = ["<!doctype html><html><head><meta charset='utf-8'>",
             "<style>",
             "body{margin:0;background:#%s;}" % pal["bg"],
             ".blk{position:absolute;box-sizing:border-box;}",
             ".blk span{font:12px monospace;color:#%s;}" % pal["fg"],
             "</style></head><body>"]

    def css(node):
        r = node["rect"]
        style = "left:%dpx;top:%dpx;width:%dpx;height:%dpx;" % (
            r["x"], r["y"], r["w"], r["h"])
        fill = resolve(node, "fill", theme)
        if fill:
            style += "background:#%s;" % fill
        stroke = node.get("stroke") or {}
        sides = stroke.get("sides") or []
        col = stroke.get("color")
        if not col:
            col = pal["border"]
        elif col in bc.ROLE_NAMES:
            col = pal[col]
        col = col.lstrip("#")
        if mode == "app":
            style += "border:1px solid #%s;" % col
        elif mode == "design" and node.get("radius"):
            rad = node["radius"]
            style += "border-radius:%spx %spx %spx %spx;" % (
                rad["tl"], rad["tr"], rad["br"], rad["bl"])
        if mode == "calm" and _calm_eligible(node, theme):
            ri = calm_r_index(r["w"], r["h"])
            style += "border-radius:%dpx;" % CALM_RADII[ri]
        return style

    def walk(node, depth):
        rid = "n%d" % node["id"]
        parts.append("<div class='blk' id='%s' style='%s'>" % (rid, css(node)))
        if node.get("text"):
            parts.append("<span>%s</span>" % node["text"])
        for c in node.get("children") or []:
            walk(c, depth + 1)
        parts.append("</div>")

    walk(root, 0)
    parts.append("</body></html>")
    return "".join(parts)


# ---------------------------------------------------------------------------
# 主流程
# ---------------------------------------------------------------------------


def _frame_ticks(tree):
    """按 16ms tick 收集动画时长，返回 0..max_ms 的 tick 列表。"""
    max_ms = 0
    for n in bc.iter_nodes(tree["root"]):
        max_ms = max(max_ms, n.get("anim_ms", 0))
    if not max_ms:
        return [0]
    return list(range(0, max_ms + 1, 16))


def render_frames(tree, mode, theme, font_src, out_dir):
    """--frames：按 16ms tick 预生成偏移表并逐帧导出 PNG。返回帧路径列表。"""
    os.makedirs(out_dir, exist_ok=True)
    ticks = _frame_ticks(tree)
    paths = []
    for i, tick in enumerate(ticks):
        buf = render_png(tree, mode, theme, font_src, frame_tick=tick)
        p = os.path.join(out_dir, "frame-%03d.png" % i)
        write_png(p, buf)
        paths.append(p)
    return paths


def play_table(tree):
    """--play：打印每 tick 的缓动进度表（确定性，可复核），不落图。"""
    rows = []
    for n in bc.iter_nodes(tree["root"]):
        ms = n.get("anim_ms", 0)
        if ms:
            rows.append((n["id"], ms, n.get("anim_ease", 1),
                         n.get("anim_mask", 0)))
    lines = ["tick_ms,node_id,anim_ms,ease,progress"]
    for tick in _frame_ticks(tree):
        for nid, ms, ease, mask in rows:
            t = anim_progress(ms, ease, tick)
            lines.append("%d,%d,%d,%d,%.4f" % (tick, nid, ms, ease, t))
    return "\n".join(lines) + "\n"


def main(argv=None):
    ap = argparse.ArgumentParser(description=".blk.json → HTML / PNG 预览")
    ap.add_argument("blkjson")
    ap.add_argument("--html", help="输出单文件 HTML 路径")
    ap.add_argument("--png", help="输出 PNG 路径")
    ap.add_argument("--mode", choices=["app", "calm", "design"], default="app")
    ap.add_argument("--theme", choices=["dark", "light", "hc"], default="dark")
    ap.add_argument("--font-src", help="font_ascii.inc 覆盖路径")
    ap.add_argument("--frames", metavar="DIR",
                    help="按 16ms tick 逐帧导出 PNG 序列到目录")
    ap.add_argument("--play", action="store_true",
                    help="打印逐 tick 缓动进度表（离线推演，不落图）")
    ap.add_argument("--verbose", action="store_true",
                    help="stderr 打印字模解析路径与字形数")
    args = ap.parse_args(argv)

    with open(args.blkjson, "r", encoding="utf-8") as f:
        tree = json.load(f)

    if not args.html and not args.png and not args.frames and not args.play:
        ap.error("至少指定 --html / --png / --frames / --play")

    if args.html:
        html = render_html(tree, args.mode, args.theme)
        os.makedirs(os.path.dirname(os.path.abspath(args.html)) or ".", exist_ok=True)
        with open(args.html, "w", encoding="utf-8") as f:
            f.write(html)
        print("wrote %s" % args.html, file=sys.stderr)

    if args.png:
        font = bc.load_font(args.font_src)
        buf = render_png(tree, args.mode, args.theme, args.font_src)
        os.makedirs(os.path.dirname(os.path.abspath(args.png)) or ".", exist_ok=True)
        n = write_png(args.png, buf)
        print("wrote %s (%dx%d, %d bytes)" % (args.png, buf.w, buf.h, n),
              file=sys.stderr)
        if args.verbose:
            fp = bc.font_path(args.font_src)
            print("font: %s (%d glyphs)" % (fp, len(font)), file=sys.stderr)

    if args.frames:
        paths = render_frames(tree, args.mode, args.theme, args.font_src,
                              args.frames)
        print("wrote %d frames to %s" % (len(paths), args.frames),
              file=sys.stderr)

    if args.play:
        sys.stdout.write(play_table(tree))

    return 0


if __name__ == "__main__":
    sys.exit(main())
