#!/usr/bin/env python3
"""gui_pixel_assert.py -- 块化 GUI 桌面帧的像素级断言（验收 §9，M0 视觉 QC）。

与串口探针互补：串口只能证明块树/布局/损伤打点正确，无法证明像素真的
落到了 LFB。本脚本直接解析 QEMU `screendump` 产出的 P6 PPM，逐点采样并
做可自动化的颜色断言，替代"靠眼睛看图"。

断言类型（checks 列表，每项 dict）：
  {"kind": "ne_bg",  "pt": (x, y)}                      该点 != 桌面底色
  {"kind": "ne_color","pt": (x, y), "color": (r,g,b)}   该点 != 指定色
  {"kind": "color",  "pt": (x, y), "color": (r,g,b)}    该点 == 指定色(容差内)
  {"kind": "pair_diff", "pts": [(x,y), (x,y)]}          两点彼此不同(容差外)

用法：
    python3 tests/gui_pixel_assert.py <frame.ppm> [--bg R,G,B] [--tol N]
    # 不带 --checks 时使用内置 M0 桌面 800x600 三条断言：
    #   1. 块边缘 (65,65) 非桌面底色（描边存在）
    #   2. 标题条内 (150,70) 与块底 (200,120) 均非底色，且二者互异
    #   3. 任务栏 (400,585) 与 Start 文字 (20,585) 非底色

退出码：0 全部通过；1 有失败；2 输入/解析错误。
"""
import argparse
import re
import sys

HERE = __file__
REPO = __file__ and __file__.rsplit("/", 1)[0].rsplit("/", 1)[0]

# 令牌 DARK（include/gui/tokens.h / tokens.c）。
DARK_BG    = (30, 30, 38)    # 0x1E1E26 桌面底色
DARK_PANEL = (42, 42, 52)   # 0x2A2A34 应用块底色
DARK_BAR   = (58, 58, 72)   # 0x3A3A48 标题栏 / 任务栏
DARK_FG    = (232, 232, 236)  # 0xE8E8EC 文字

# 令牌 LIGHT（tokens.c tok_light）。
LIGHT_BG    = (236, 236, 241)  # 0xECECF1
LIGHT_PANEL = (221, 221, 230)  # 0xDDDDE6
LIGHT_BAR   = (200, 200, 214)  # 0xC8C8D6
LIGHT_FG    = (24, 24, 32)     # 0x181820

# 令牌 HIGH CONTRAST（tokens.c tok_hc）。
HC_BG    = (0, 0, 0)
HC_PANEL = (16, 16, 16)
HC_BAR   = (32, 32, 32)
HC_FG    = (255, 255, 255)
HC_BORDER = (255, 255, 0)

# M0 桌面默认三条验收断言（几何见 `gui geom` 转储：块 64,64 320x200；
# 标题条 y=64 h=24；正文 y=88 起；任务栏 y=568 h=32）。
DEFAULT_CHECKS = [
    # 1. 块边缘一像素必须 != 桌面底色（描边/块体存在）。
    {"kind": "ne_bg", "pt": (65, 65)},
    # 2. 标题条内部与块底均非底色，且标题条与内容区可分辨。
    {"kind": "ne_bg", "pt": (150, 70)},
    {"kind": "ne_bg", "pt": (200, 120)},
    {"kind": "pair_diff", "pts": [(150, 70), (200, 120)]},
    # 3. 任务栏底色与 Start 文字均与底色不同。
    {"kind": "ne_bg", "pt": (400, 585)},
    {"kind": "ne_bg", "pt": (20, 585)},
]

# 主题 live 重着色断言：桌面底 / 块体 / 任务栏三点必须落在目标令牌值，
# 且必须仍与暗色令牌不同（防止切主题后画面仍固化暗色）。
def _theme_checks(bg, panel, bar, dark_bg, dark_panel, dark_bar):
    return [
        {"kind": "color", "pt": (300, 300), "color": bg},
        {"kind": "ne_color", "pt": (300, 300), "color": dark_bg},
        {"kind": "color", "pt": (200, 120), "color": panel},
        {"kind": "ne_color", "pt": (200, 120), "color": dark_panel},
        {"kind": "color", "pt": (400, 585), "color": bar},
        {"kind": "ne_color", "pt": (400, 585), "color": dark_bar},
    ]


THEME_LIGHT_CHECKS = _theme_checks(
    LIGHT_BG, LIGHT_PANEL, LIGHT_BAR, DARK_BG, DARK_PANEL, DARK_BAR)
THEME_HC_CHECKS = _theme_checks(
    HC_BG, HC_PANEL, HC_BAR, DARK_BG, DARK_PANEL, DARK_BAR)

# 开始菜单浮层断言：菜单容器 (8,400,180,~96) 内必须同时存在 ≥100 个
# 任务栏/菜单底色像素与 ≥100 个前景文字像素，且都不是桌面底色。
MENU_CHECKS = [
    {"kind": "region_count", "rect": (8, 400, 180, 96),
     "color": DARK_BAR, "min": 100, "label": "menu bar"},
    {"kind": "region_count", "rect": (8, 400, 180, 96),
     "color": DARK_FG, "min": 100, "label": "menu fg"},
]

# 11b 拖拽残影断言：A 从旧矩形 (64,300,120,80) 拖走后，该区域必须回到
# 桌面底色；允许极少数边缘像素差异（max_bad 容差像素数）。
DRAG_CHECKS = [
    {"kind": "region_bg", "rect": (64, 300, 120, 80),
     "max_bad": 50, "label": "drag old rect"},
]

# D7 超框位图断言：200 宽位图塞进 80 宽父块，父框外右条 (520,80,119,40)
# 必须保持桌面底色，不允许溢出像素。
CROP_CHECKS = [
    {"kind": "region_bg", "rect": (520, 80, 119, 40),
     "max_bad": 20, "label": "crop outside parent"},
]

# 记事本：文本区块 (144,96,512,272) 必须是面板底色，且区别于桌面底色。
NOTEPAD_CHECKS = [
    {"kind": "ne_bg", "pt": (300, 200)},
    {"kind": "region_count", "rect": (144, 96, 512, 272),
     "color": DARK_PANEL, "min": 100, "label": "notepad body panel"},
]

# 画布演示：棋盘格左上 (158,104) 与渐变段 (230,120) 互异，棋盘前景存在。
CANVAS_CHECKS = [
    {"kind": "ne_bg", "pt": (158, 104)},
    {"kind": "pair_diff", "pts": [(158, 104), (230, 120)]},
    {"kind": "region_count", "rect": (158, 104, 200, 160),
     "color": DARK_FG, "min": 20, "label": "canvas checker fg"},
]

# 资源管理器：列表区 (144,124,512,280) 必须是面板底色，区别于桌面底色。
FILES_CHECKS = [
    {"kind": "ne_bg", "pt": (300, 200)},
    {"kind": "region_count", "rect": (144, 124, 512, 280),
     "color": DARK_PANEL, "min": 50, "label": "files list panel"},
]

# 图片查看器：视口 (152,72,560,400) 内存在非桌面底色的真实图像内容。
IMAGES_CHECKS = [
    {"kind": "ne_bg", "pt": (300, 200)},
    {"kind": "ne_bg", "pt": (500, 350)},
]

# 浏览器 v0：内容区 (120,96,560,300) 为面板底色，且存在真实文本前景像素。
BROWSER_CHECKS = [
    {"kind": "ne_bg", "pt": (300, 200)},
    {"kind": "region_count", "rect": (120, 96, 560, 300),
     "color": DARK_FG, "min": 20, "label": "browser text fg"},
]

CHECK_GROUPS = {
    "dark": DEFAULT_CHECKS,
    "light": THEME_LIGHT_CHECKS,
    "hc": THEME_HC_CHECKS,
    "menu": MENU_CHECKS,
    "drag": DRAG_CHECKS,
    "crop": CROP_CHECKS,
    "notepad": NOTEPAD_CHECKS,
    "canvas": CANVAS_CHECKS,
    "files": FILES_CHECKS,
    "images": IMAGES_CHECKS,
    "browser": BROWSER_CHECKS,
}

GROUP_BG = {
    "dark": DARK_BG,
    "light": LIGHT_BG,
    "hc": HC_BG,
    "menu": DARK_BG,
    "drag": DARK_BG,
    "crop": DARK_BG,
    "notepad": DARK_BG,
    "canvas": DARK_BG,
    "files": DARK_BG,
    "images": DARK_BG,
    "browser": DARK_BG,
}


class Ppm(object):
    """P6 binary PPM 只读解析（与 gfx24_regress.py 同一格式）。"""

    def __init__(self, path):
        with open(path, "rb") as fh:
            data = fh.read()
        m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+(\d+)\s", data)
        if not m:
            raise RuntimeError("not a P6 PPM: %r" % data[:32])
        self.w, self.h, self.mx = (int(v) for v in m.groups())
        self.raw = data[m.end():m.end() + self.w * self.h * 3]
        if len(self.raw) != self.w * self.h * 3:
            raise RuntimeError("truncated PPM: need %d bytes" %
                               (self.w * self.h * 3))

    def px(self, x, y):
        if not (0 <= x < self.w and 0 <= y < self.h):
            raise RuntimeError("point (%d,%d) outside %dx%d" %
                               (x, y, self.w, self.h))
        i = (y * self.w + x) * 3
        return tuple(self.raw[i:i + 3])


def _close(a, b, tol):
    return all(abs(a[k] - b[k]) <= tol for k in range(3))


def _run_checks(ppm, checks, bg, tol):
    failures = []
    passed = 0
    for idx, ch in enumerate(checks):
        kind = ch["kind"]
        if kind == "ne_bg":
            pt = tuple(ch["pt"])
            got = ppm.px(*pt)
            ok = not _close(got, bg, tol)
            msg = "pt=%s got=%s want != bg%s" % (pt, got, bg)
        elif kind == "ne_color":
            pt = tuple(ch["pt"])
            got = ppm.px(*pt)
            ok = not _close(got, tuple(ch["color"]), tol)
            msg = "pt=%s got=%s want != %s" % (pt, got, tuple(ch["color"]))
        elif kind == "color":
            pt = tuple(ch["pt"])
            got = ppm.px(*pt)
            ok = _close(got, tuple(ch["color"]), tol)
            msg = "pt=%s got=%s want ~= %s" % (pt, got, tuple(ch["color"]))
        elif kind == "pair_diff":
            pts = [tuple(p) for p in ch["pts"]]
            g0, g1 = ppm.px(*pts[0]), ppm.px(*pts[1])
            ok = not _close(g0, g1, tol)
            msg = "pts=%s got=(%s,%s) want differ" % (pts, g0, g1)
        elif kind == "region_count":
            rx, ry, rw, rh = ch["rect"]
            want = tuple(ch["color"])
            n = 0
            for y in range(ry, min(ry + rh, ppm.h)):
                for x in range(rx, min(rx + rw, ppm.w)):
                    if _close(ppm.px(x, y), want, tol):
                        n += 1
            ok = n >= int(ch["min"])
            msg = "%s rect=%s count=%d want >=%d ~= %s" % (
                ch.get("label", "region"), ch["rect"], n, ch["min"], want)
        elif kind == "region_bg":
            rx, ry, rw, rh = ch["rect"]
            bad = 0
            for y in range(ry, min(ry + rh, ppm.h)):
                for x in range(rx, min(rx + rw, ppm.w)):
                    if not _close(ppm.px(x, y), bg, tol):
                        bad += 1
            ok = bad <= int(ch["max_bad"])
            msg = "%s rect=%s non-bg=%d want <=%d bg=%s" % (
                ch.get("label", "region"), ch["rect"], bad,
                int(ch["max_bad"]), bg)
        else:
            failures.append("check %d: unknown kind %r" % (idx, kind))
            continue

        if ok:
            passed += 1
            print("  ok  [%d] %s" % (idx, msg))
        else:
            failures.append("check %d: %s" % (idx, msg))
            print("  FAIL[%d] %s" % (idx, msg))
    return passed, failures


def main():
    ap = argparse.ArgumentParser(description="GUI desktop PPM pixel asserts")
    ap.add_argument("ppm", help="QEMU screendump P6 PPM")
    ap.add_argument("--group", choices=sorted(CHECK_GROUPS.keys()),
                    default="dark", help="check group (default dark)")
    ap.add_argument("--bg", default=None,
                    help="desktop bg R,G,B (default: group token bg)")
    ap.add_argument("--tol", type=int, default=6,
                    help="per-channel tolerance (default 6)")
    args = ap.parse_args()

    checks = CHECK_GROUPS[args.group]
    bg_raw = args.bg or ",".join(str(v) for v in GROUP_BG[args.group])
    try:
        bg = tuple(int(v) for v in bg_raw.split(","))
        if len(bg) != 3:
            raise ValueError
    except ValueError:
        print("FAIL: --bg must be R,G,B ints")
        return 2

    try:
        ppm = Ppm(args.ppm)
    except (OSError, RuntimeError) as e:
        print("FAIL: %s" % e)
        return 2

    print("frame=%dx%d group=%s bg=%s tol=%d" %
          (ppm.w, ppm.h, args.group, bg, args.tol))
    passed, failures = _run_checks(ppm, checks, bg, args.tol)
    print("== %d/%d checks passed ==" % (passed, len(checks)))
    if failures:
        print("RESULT=FAIL")
        for f in failures:
            print("  - " + f)
        return 1
    print("RESULT=PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
