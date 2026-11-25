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
        r, g, b = bytes.fromhex(color)
        off = x * 3
        self.rows[row][off:off + w * 3] = bytearray([r, g, b]) * w

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


def draw_text(buf, node, theme, font):
    text = node.get("text")
    if not text:
        return
    scale = (node.get("font") or {}).get("scale", 1)
    r = node["rect"]
    fg = resolve(node, "fg", theme, bc.palette(theme)["fg"])
    content_w = r["w"] - 8
    line = fit_text(text, content_w, scale, font)
    ox = r["x"] + 4
    oy = r["y"] + max(0, (r["h"] - 16 * scale) // 2)
    col = bytes.fromhex(fg)
    cx = ox
    for ch in line:
        o = ord(ch)
        glyph = font.get(o)
        if glyph:
            for ry, byte in enumerate(glyph):
                for bit in range(8):
                    if byte & (0x80 >> bit):
                        for sy in range(scale):
                            for sx in range(scale):
                                px = cx + bit * scale + sx
                                py = oy + ry * scale + sy
                                if 0 <= py < buf.h and 0 <= px < buf.w:
                                    off = px * 3
                                    buf.rows[py][off:off + 3] = bytes(col)
        cx += 8 * scale


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


def render_png(tree, mode, theme, font_src=None):
    root = tree["root"]
    fr = root["rect"]
    buf = PixBuf(fr["w"], fr["h"], bc.palette(theme)["bg"])
    font = bc.load_font(font_src)

    def walk(node, parent):
        r = node["rect"]
        kind = node["kind"]
        fill = resolve(node, "fill", theme)
        if kind in ("column", "row", "stack", "free", "root", "rect",
                    "button", "input", "panel"):
            if fill:
                if mode == "calm" and _calm_eligible(node, theme):
                    ri = calm_r_index(r["w"], r["h"])
                    buf.fill_round(r["x"], r["y"], r["w"], r["h"],
                                   CALM_RADII[ri], fill)
                elif mode == "design" and node.get("radius"):
                    buf.fill_round(r["x"], r["y"], r["w"], r["h"], 0, fill,
                                   node["radius"])
                else:
                    buf.fill_rect(r["x"], r["y"], r["w"], r["h"], fill)

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
            walk(c, node)

    walk(root, None)
    return buf


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


def main(argv=None):
    ap = argparse.ArgumentParser(description=".blk.json → HTML / PNG 预览")
    ap.add_argument("blkjson")
    ap.add_argument("--html", help="输出单文件 HTML 路径")
    ap.add_argument("--png", help="输出 PNG 路径")
    ap.add_argument("--mode", choices=["app", "calm", "design"], default="app")
    ap.add_argument("--theme", choices=["dark", "light", "hc"], default="dark")
    ap.add_argument("--font-src", help="font_ascii.inc 覆盖路径")
    ap.add_argument("--verbose", action="store_true",
                    help="stderr 打印字模解析路径与字形数")
    args = ap.parse_args(argv)

    with open(args.blkjson, "r", encoding="utf-8") as f:
        tree = json.load(f)

    if not args.html and not args.png:
        ap.error("至少指定 --html 或 --png")

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

    return 0


if __name__ == "__main__":
    sys.exit(main())
