"""preview.py 离线渲染专项单测（cap2/cap3 可复核口径）。

覆盖用户点名清单：
- alpha 抖动覆盖率（ordered16 Bayer / none 最近档）
- 9-slice 阴影拼缝（块体外、右下光源；HC 换边框）
- 逐角 / pill / auto(阶梯) 半径解析
- wrap 末行省略（char / word）
- 四档字高字号
- 7 条缓动落点（端点 / 中点 / 过冲 / step）
- popover 四向翻转与 8px 安全边距钳制
- 内置图标数量与 tint 角色
- scrollbar 比例
- cap3 arc vs squircle 角部像素差 ≥ 3/255（像素数口径）

断言均基于几何（块 rect、角色色、确定性函数输出），不写死整图 RGB。
"""

import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL_DIR = os.path.dirname(HERE)
sys.path.insert(0, TOOL_DIR)

import blkcommon  # noqa: E402
bc = blkcommon
import preview  # noqa: E402


def _tree(children, w=320, h=240):
    return {"schema": "catos.blk/1.0",
            "root": {"id": 0, "kind": "root",
                     "rect": {"x": 0, "y": 0, "w": w, "h": h},
                     "children": children}}


def _count_color(buf, rect, color):
    x, y, w, h = rect
    n = 0
    for row in range(max(0, y), min(buf.h, y + h)):
        for col in range(max(0, x), min(buf.w, x + w)):
            off = col * 3
            if (buf.rows[row][off], buf.rows[row][off + 1],
                    buf.rows[row][off + 2]) == tuple(bytes.fromhex(color)):
                n += 1
    return n


class TestDitherCoverage(unittest.TestCase):
    def test_ordered16_coverage_near_alpha(self):
        """alpha=128 抖动点亮比例应逼近 128/255（≈0.502），容差 ±12%。"""
        node = {"id": 1, "kind": "rect", "rect": {"x": 20, "y": 20,
                "w": 40, "h": 40}, "fill": "panel", "alpha": 128,
                "alpha_mode": 0}
        buf = preview.render_png(_tree([node]), "app", "dark")
        panel = bc.palette("dark")["panel"]
        lit = _count_color(buf, (20, 20, 40, 40), panel)
        frac = lit / (40 * 40)
        self.assertAlmostEqual(frac, 128 / 255, delta=0.12)

    def test_none_mode_nearest_step_no_dither(self):
        """alpha_mode=none：覆盖 ≥0.5 取最近档整面点亮，不做 Bayer 抖动。"""
        node = {"id": 1, "kind": "rect", "rect": {"x": 20, "y": 20,
                "w": 40, "h": 40}, "fill": "panel", "alpha": 128,
                "alpha_mode": 1}
        buf = preview.render_png(_tree([node]), "app", "dark")
        panel = bc.palette("dark")["panel"]
        # 1px 描边占去外圈，内部 38×38 应整面点亮（无 Bayer 棋盘格）。
        lit = _count_color(buf, (22, 22, 36, 36), panel)
        self.assertEqual(lit, 36 * 36)

    def test_alpha_inheritance_multiply(self):
        """子树乘法继承：父 192 × 子 128 / 255 ≈ 96，点亮比例 ≈ 96/255。"""
        child = {"id": 2, "kind": "rect", "rect": {"x": 20, "y": 20,
                 "w": 40, "h": 40}, "fill": "panel", "alpha": 128}
        parent = {"id": 1, "kind": "column", "rect": {"x": 0, "y": 0,
                  "w": 320, "h": 240}, "alpha": 192, "children": [child]}
        buf = preview.render_png(_tree([parent]), "app", "dark")
        panel = bc.palette("dark")["panel"]
        lit = _count_color(buf, (20, 20, 40, 40), panel)
        frac = lit / 1600
        self.assertAlmostEqual(frac, (192 * 128 // 255) / 255, delta=0.15)


class TestElevationShadow(unittest.TestCase):
    def test_shadow_outside_block_body(self):
        """elevation=2 阴影落在块体外（右下），与无阴影帧存在块外差异。"""
        base = {"id": 1, "kind": "rect", "rect": {"x": 40, "y": 40,
                "w": 80, "h": 60}, "fill": "panel"}
        raised = dict(base, elevation=2)
        b0 = preview.render_png(_tree([base]), "app", "dark")
        b1 = preview.render_png(_tree([raised]), "app", "dark")
        outside = [(x, y) for y in range(0, b1.h) for x in range(0, b1.w)
                   if not (40 <= x < 120 and 40 <= y < 100)]
        diff = sum(1 for x, y in outside if b0.rows[y] != b1.rows[y]
                   and b1.rows[y][x * 3:x * 3 + 3] !=
                   b0.rows[y][x * 3:x * 3 + 3])
        self.assertGreater(diff, 0, "阴影应在块体外产生暗带")

    def test_hc_theme_uses_border_instead_of_shadow(self):
        node = {"id": 1, "kind": "rect", "rect": {"x": 40, "y": 40,
                "w": 80, "h": 60}, "fill": "panel", "elevation": 3}
        buf = preview.render_png(_tree([node]), "app", "hc")
        border = bc.palette("hc")["border"]
        edge = _count_color(buf, (40, 40, 80, 1), border)
        self.assertGreater(edge, 0)


class TestCornerRadius(unittest.TestCase):
    def test_per_corner_values_passthrough(self):
        node = {"radius": {"tl": 16, "tr": 16, "br": 0, "bl": 0}}
        w, h = 160, 48
        self.assertEqual(preview.corner_radius("tl", node, w, h), 16)
        self.assertEqual(preview.corner_radius("br", node, w, h), 0)

    def test_pill_is_half_short_edge(self):
        node = {"radius": {"tl": 128, "tr": 128, "br": 128, "bl": 128}}
        self.assertEqual(preview.corner_radius("tl", node, 160, 48), 24)

    def test_auto_uses_calm_ladder(self):
        node = {"radius": {"tl": 255, "tr": 255, "br": 255, "bl": 255}}
        r = preview.corner_radius("tl", node, 160, 48)
        self.assertIn(r, preview.CALM_RADII)


class TestWrapEllipsis(unittest.TestCase):
    def test_char_wrap_two_lines_ellipsis(self):
        lines = preview.wrap_text("abcdefghijklmnopqrst", 24, 8, 2, 1)
        self.assertEqual(len(lines), 2)
        self.assertTrue(lines[-1].endswith("..."))

    def test_word_wrap_breaks_on_space(self):
        lines = preview.wrap_text("alpha bravo charlie delta", 24, 8, 2, 2)
        self.assertEqual(len(lines), 2)
        self.assertTrue(lines[-1].endswith("..."))

    def test_single_line_no_wrap(self):
        lines = preview.wrap_text("Hi", 200, 8, 0, 0)
        self.assertEqual(lines, ["Hi"])


class TestFontMetrics(unittest.TestCase):
    def test_four_sizes_metrices(self):
        # 0 默认 body / 1 caption 8 / 2 body 16 / 3 title 24 / 4 display 32
        self.assertEqual(preview.FONT_METRICS[1], (8, 8))
        self.assertEqual(preview.FONT_METRICS[2], (8, 16))
        self.assertEqual(preview.FONT_METRICS[3], (12, 24))
        self.assertEqual(preview.FONT_METRICS[4], (16, 32))

    def test_text_draws_all_sizes_without_error(self):
        for size in (1, 2, 3, 4):
            node = {"id": 1, "kind": "text",
                    "rect": {"x": 8, "y": 8, "w": 120, "h": 32},
                    "text": "Ag", "font_size": size}
            buf = preview.render_png(_tree([node], 200, 64), "app", "dark")
            self.assertEqual((buf.w, buf.h), (200, 64))


class TestEasing(unittest.TestCase):
    def test_endpoints_all_easings(self):
        for ease in range(1, 8):
            self.assertAlmostEqual(preview.ease_func(ease, 0.0), 0.0,
                                   places=6, msg=str(ease))
            self.assertAlmostEqual(preview.ease_func(ease, 1.0), 1.0,
                                   places=6, msg=str(ease))

    def test_linear_midpoint(self):
        self.assertAlmostEqual(preview.ease_func(1, 0.5), 0.5)

    def test_out_back_overshoots(self):
        vals = [preview.ease_func(5, t / 20) for t in range(21)]
        self.assertGreater(max(vals), 1.0)

    def test_step_holds_zero_until_end(self):
        self.assertEqual(preview.ease_func(7, 0.5), 0.0)
        self.assertEqual(preview.ease_func(7, 1.0), 1.0)

    def test_anim_progress_tick_table(self):
        # 144ms / 16ms tick：端点进度 0 → 1。
        self.assertAlmostEqual(preview.anim_progress(144, 1, 0), 0.0)
        self.assertAlmostEqual(preview.anim_progress(144, 1, 144), 1.0)


class TestPopoverLayout(unittest.TestCase):
    ANCHOR = {"x": 160, "y": 120, "w": 80, "h": 24}
    BOUNDS = {"x": 0, "y": 0, "w": 400, "h": 300}

    def test_four_prefer_directions(self):
        seen = {}
        for prefer, name in ((1, "up"), (2, "down"), (4, "left"),
                             (8, "right")):
            rect, d = preview.pop_layout(self.ANCHOR, 110, 48, prefer,
                                         120, self.BOUNDS)
            seen[name] = (rect, d)
        self.assertEqual(seen["up"][1], "up")
        self.assertEqual(seen["down"][1], "down")
        self.assertEqual(seen["left"][1], "left")
        self.assertEqual(seen["right"][1], "right")

    def test_clamped_to_margin(self):
        rect, _ = preview.pop_layout({"x": 0, "y": 0, "w": 20, "h": 20},
                                    110, 48, 1, 120, self.BOUNDS)
        self.assertGreaterEqual(rect["x"], 8)
        self.assertGreaterEqual(rect["y"], 8)

    def test_max_h_shrinks(self):
        rect, _ = preview.pop_layout(self.ANCHOR, 110, 200, 2, 120,
                                    self.BOUNDS)
        self.assertEqual(rect["h"], 120)


class TestIconTint(unittest.TestCase):
    def test_builtin_icons_at_least_eight(self):
        self.assertGreaterEqual(len(preview.ICON_NAMES), 8)
        for name in preview.ICON_NAMES:
            self.assertGreater(len(preview.icon_pixels(name, 16)), 0)

    def test_icon_size_double_points(self):
        small = preview.icon_pixels("clock", 16)
        big = preview.icon_pixels("clock", 32)
        self.assertGreater(len(big), len(small))

    def test_tint_role_colors(self):
        pal = bc.palette("dark")
        cases = {1: pal["fg"], 2: pal["bg"], 3: pal["accent"],
                 4: pal["border"]}
        for role, col in cases.items():
            node = {"tint_role": role}
            self.assertEqual(preview._tint_color(node, "dark"), col)


class TestScrollbar(unittest.TestCase):
    def test_ratio_thumb(self):
        meta = preview.scrollbar_thumb(100, 400)
        self.assertIsNotNone(meta)
        self.assertAlmostEqual(meta["ratio"], 100 / 400, places=3)

    def test_no_thumb_when_fits(self):
        self.assertIsNone(preview.scrollbar_thumb(400, 100))


class TestCap3ShapeDiff(unittest.TestCase):
    def test_arc_vs_squircle_coverage_diff(self):
        """R-7.5-03：ARC/SQ5 角中间段覆盖率差 ≥3/255，证明几何确实不同。

        口径：4×4 面积子采样取角部 r×r 区域逐点覆盖率，统计平均绝对差；
        不经过 8/16 二值化阈值（阈值会把临界像素抹成一致，掩盖几何差）。
        """
        r = 20
        arc = [preview.shape_coverage(px, py, r, 64, 64, "tl", 0, 5.0)
               for px in range(r) for py in range(r)]
        sq = [preview.shape_coverage(px, py, r, 64, 64, "tl", 1, 5.0)
              for px in range(r) for py in range(r)]
        mean_diff = sum(abs(a - b) for a, b in zip(arc, sq)) / len(arc)
        self.assertGreaterEqual(mean_diff, 3 / 255)

    def test_squircle_n_changes_coverage(self):
        """n=5.0 与 n=4.5 在角部覆盖率不同（确定性面积采样）。"""
        cov5 = preview.shape_coverage(2, 2, 20, 64, 64, "tl", 1, 5.0)
        cov45 = preview.shape_coverage(2, 2, 20, 64, 64, "tl", 1, 4.5)
        self.assertNotEqual(cov5, cov45)


if __name__ == "__main__":
    unittest.main()
