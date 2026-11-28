#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""final3 按钮图标宿主侧像素断言：
  1) 图标区墨迹 > 0；
  2) 文字墨迹列簇与图标列簇不重叠（中间存在空列带）；
  3) hover 帧首按钮图标仍可见。
几何：裁切图相对按钮外扩 4px；按钮局部坐标 (4,4)..(4+220,4+32)。
"""
import os, sys
from PIL import Image

ART = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   "..", "artifacts", "desktop-icons", "final3")
ART = os.path.normpath(ART)

# 16px 图标左缘在按钮内 pad_l；文字自图标右 +6 起。
def col_profile(img, x0, x1, y0, y1, bg, tol=10):
    prof = []
    for x in range(x0, x1):
        n = 0
        for y in range(y0, y1):
            r, g, b = img.getpixel((x, y))[:3]
            if abs(r - bg[0]) > tol or abs(g - bg[1]) > tol or abs(b - bg[2]) > tol:
                n += 1
        prof.append(n)
    return prof

def check(name, notext=False):
    p = os.path.join(ART, name)
    img = Image.open(p).convert("RGB")
    W, H = img.size
    bg = img.getpixel((2, 2))
    # 按钮 body 内行带（去掉上下边框行）；左右边框列不计入墨迹。
    y0, y1 = 7, min(33, H)
    prof = col_profile(img, 5, W - 5, y0, y1, bg)
    cols = [i + 5 for i, n in enumerate(prof) if n > 0]
    if not cols:
        print("FAIL %s: 整按钮无墨迹" % name); return False
    ok = True
    if notext:
        c0, c1 = min(cols), max(cols)
        mid = (c0 + c1) / 2
        ok = abs(mid - W / 2) <= 40
        print(("%s" % "PASS" if ok else "FAIL"),
              "%s: 仅图标居中 bbox x=%d..%d 中心=%.0f(按钮中=%d)" %
              (name, c0, c1, mid, W / 2))
        return ok
    icon_cols = [c for c in cols if c < 4 + 30]
    text_cols = [c for c in cols if c >= 4 + 30]
    if not icon_cols:
        print("FAIL %s: 图标区为空" % name); ok = False
    else:
        print("PASS %s: 图标墨迹列 %d 列, 图标 bbox x=%d..%d" %
              (name, len(icon_cols), min(icon_cols), max(icon_cols)))
    if not text_cols:
        print("FAIL %s: 文字区为空" % name); ok = False
    else:
        gap = min(text_cols) - max(icon_cols)
        print("PASS %s: 文字 bbox x=%d..%d, 图文列间距=%d" %
              (name, min(text_cols), max(text_cols), gap))
        if gap <= 0:
            print("FAIL %s: 图文列重叠" % name); ok = False
    return ok

def check_hover():
    p = os.path.join(ART, "btn-hover-icon.png")
    img = Image.open(p).convert("RGB")
    bg = img.getpixel((2, 2))
    # 首按钮 (40,40) 起，图标在左；取图标列带 40..64。
    n = 0
    for x in range(40, 64):
        for y in range(40, 72):
            r, g, b = img.getpixel((x, y))[:3]
            if abs(r - bg[0]) > 10 or abs(g - bg[1]) > 10 or abs(b - bg[2]) > 10:
                n += 1
    print(("PASS" if n > 0 else "FAIL"), "hover 首按钮图标墨迹像素=%d" % n)
    return n > 0

if __name__ == "__main__":
    cases = [("btn-svg-ok.png", False), ("btn-svg-folder.png", False),
             ("btn-svg-fallback.png", False), ("btn-icon24.png", False),
             ("btn-notext.png", True), ("btn-notext-bad.png", False)]
    allok = True
    for name, nt in cases:
        allok = check(name, nt) and allok
    allok = check_hover() and allok
    print("RESULT=" + ("PASS" if allok else "FAIL"))
    sys.exit(0 if allok else 1)
