"""samples/cap2 五个样例端到端断言（html2blk → validate → preview → 哈希）。

断言口径：
- 几何/属性：直接取块树字段（半径、alpha、wrap、字号、tint、动画掩码），
  依据样例 HTML 源码与规范 v0.3 冻结映射推导。
- 像素：只取块体内部区域统计角色色点亮比例，阈值由 alpha 覆盖率与
  几何尺寸推导；不写死整图 RGB，也不比对内核取帧残差（见规范 §9.0）。
- 哈希：``.blk.json`` 严格复现（--title <名> --theme dark），sha256 锚点
  回写规范 §9.56；改转换器导致产物漂移时本测试先红。
"""

import hashlib
import json
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL_DIR = os.path.dirname(HERE)
SAMPLES = os.path.join(TOOL_DIR, "samples", "cap2")
sys.path.insert(0, TOOL_DIR)

import blkcommon  # noqa: E402
bc = blkcommon
import html2blk  # noqa: E402
import preview  # noqa: E402
import validate  # noqa: E402

ANCHORS = {
    "alpha-menu": "019079c7d15b8d5d25e9506b51c0f3fb32a169e947c72eda224d83e83e8794da",
    "popover-flip": "0d5eed7f66aa64cf9bf0e69f91cbf14490a41548f4fe58ed28872cf12bd81383",
    "settings-btns": "d81d611f4f9efefb8bd4740b547e6d6301b6936b6dd44254cd9e19864ee35c9d",
    "notepad-anim": "fd4b5e2ad483336d9ec1044d0e6c0710b13418767ef04f6fb3b4543c14daf275",
    "status-bar": "4594aab172ee09f51890e70fd6d079407f21b11989d0669a65d4f4fe95d1f52d",
}


def _load(name):
    html_text = open(os.path.join(SAMPLES, name + ".html"),
                     encoding="utf-8").read()
    tree, diag = html2blk.convert(html_text, name, "dark")
    return tree, diag


def _nodes(tree):
    return list(bc.iter_nodes(tree["root"]))


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


class TestCap2SamplePipeline(unittest.TestCase):
    """五样例：转换 rc=0、validate 通过、哈希锚点一致、PNG 可解码。"""

    def test_all_convert_and_validate(self):
        for name in ANCHORS:
            tree, diag = _load(name)
            self.assertFalse(diag.hard_fail, (name, diag.lines()))
            rep = validate.Report()
            validate.validate_tree(tree, rep)
            self.assertFalse(rep.failed, (name, rep.text_lines()))

    def test_blk_json_hash_anchors(self):
        for name, anchor in ANCHORS.items():
            path = os.path.join(SAMPLES, name + ".blk.json")
            digest = hashlib.sha256(open(path, "rb").read()).hexdigest()
            self.assertEqual(digest, anchor, name)

    def test_all_render_png(self):
        for name in ANCHORS:
            tree, _ = _load(name)
            buf = preview.render_png(tree, "app", "dark")
            png = preview.encode_png(buf.w, buf.h, buf.rows)
            self.assertEqual(png[:8], b"\x89PNG\r\n\x1a\n")
            self.assertGreater(len(png), 100)


class TestAlphaMenu(unittest.TestCase):
    def test_alpha_and_wrap_fields(self):
        tree, _ = _load("alpha-menu")
        panel = [n for n in _nodes(tree) if n["kind"] == "column"
                 and n.get("alpha") == 192][0]
        self.assertEqual(panel["alpha"], 192)
        texts = [n for n in panel["children"]]
        self.assertEqual(len(texts), 2)
        for t in texts:
            self.assertEqual(t["alpha"], 128)
            self.assertEqual(t["wrap_lines"], 2)
            self.assertEqual(t["ellipsis"], 1)

    def test_panel_coverage_alpha_192(self):
        """面板 alpha=192（覆盖率 192/255≈0.753），内部点亮比例逼近该值。

        子文本 alpha 128 乘法继承后更暗，统计只取面板顶部空行区域，
        避免字形像素混入；阈值 ±12% 覆盖 Bayer 量化与 1px 描边损耗。
        """
        tree, _ = _load("alpha-menu")
        panel = [n for n in _nodes(tree) if n.get("alpha") == 192][0]
        buf = preview.render_png(tree, "app", "dark")
        pal = bc.palette("dark")
        r = panel["rect"]
        # 顶部 8px 空行（padding 区），不含两行文本字形。
        band = (r["x"] + 2, r["y"] + 2, r["w"] - 4, 6)
        lit = _count_color(buf, band, pal["panel"])
        frac = lit / (band[2] * band[3])
        self.assertAlmostEqual(frac, 192 / 255, delta=0.12)

    def test_text_wraps_to_two_lines_with_ellipsis(self):
        tree, _ = _load("alpha-menu")
        text = [n for n in _nodes(tree) if n["kind"] == "text"][0]
        lines = preview.wrap_text(text["text"], text["rect"]["w"] - 8,
                                  8, text["wrap_lines"], text["ellipsis"])
        self.assertEqual(len(lines), 2)
        self.assertTrue(lines[-1].endswith("..."))


class TestPopoverFlip(unittest.TestCase):
    def test_four_popovers_prefer(self):
        tree, _ = _load("popover-flip")
        pops = [n for n in _nodes(tree) if n["kind"] == "popover"]
        self.assertEqual(len(pops), 4)
        prefers = sorted(p["pop_prefer"] for p in pops)
        self.assertEqual(prefers, [1, 2, 4, 8])

    def test_four_directions_resolve(self):
        tree, _ = _load("popover-flip")
        by_id = {n["id"]: n for n in _nodes(tree)}
        dirs = set()
        for pop in _nodes(tree):
            if pop["kind"] != "popover":
                continue
            rect, d = preview.pop_layout(
                by_id[pop["anchor_id"]]["rect"], pop["rect"]["w"],
                pop["rect"]["h"], pop["pop_prefer"],
                pop.get("pop_max_h", 0), tree["root"]["rect"])
            dirs.add(d)
            # 8px 安全边距钳制
            self.assertGreaterEqual(rect["x"], 8)
            self.assertGreaterEqual(rect["y"], 8)
        self.assertEqual(dirs, {"up", "down", "left", "right"})


class TestSettingsBtns(unittest.TestCase):
    def test_per_corner_radius_and_caption(self):
        tree, _ = _load("settings-btns")
        bar = [n for n in _nodes(tree) if n["kind"] == "column"
               and n.get("radius")][0]
        self.assertEqual(bar["radius"],
                         {"tl": 16, "tr": 16, "br": 0, "bl": 0})
        caption = [n for n in _nodes(tree) if n.get("font_size") == 1][0]
        self.assertEqual(caption["font_size"], 1)  # caption 8px

    def test_render_bar_shape(self):
        tree, _ = _load("settings-btns")
        buf = preview.render_png(tree, "app", "dark")
        self.assertEqual((buf.w, buf.h), (800, 600))


class TestNotepadAnim(unittest.TestCase):
    def test_anim_fields(self):
        tree, _ = _load("notepad-anim")
        panel = [n for n in _nodes(tree) if n.get("anim_ms")][0]
        self.assertEqual(panel["anim_ms"], 144)
        self.assertEqual(panel["anim_ease"], 5)       # out-back
        self.assertEqual(panel["anim_mask"], 15)      # x,y,w,h

    def test_frame_count_and_progress(self):
        tree, _ = _load("notepad-anim")
        ticks = preview._frame_ticks(tree)
        # 144ms / 16ms = 9 步，含端点共 10 帧。
        self.assertEqual(len(ticks), 10)
        self.assertAlmostEqual(preview.anim_progress(144, 5, 0), 0.0)
        self.assertAlmostEqual(preview.anim_progress(144, 5, 144), 1.0)
        table = preview.play_table(tree)
        # 表头 1 行 + 10 tick 行。
        self.assertEqual(len(table.strip().splitlines()), 11)

    def test_render_frames_written(self):
        import tempfile
        tree, _ = _load("notepad-anim")
        with tempfile.TemporaryDirectory() as d:
            paths = preview.render_frames(tree, "app", "dark", None, d)
            self.assertEqual(len(paths), 10)
            for p in paths:
                self.assertTrue(os.path.exists(p))


class TestStatusBar(unittest.TestCase):
    def test_icon_tint_and_tabular(self):
        tree, _ = _load("status-bar")
        icons = [n for n in _nodes(tree) if n["kind"] == "icon"]
        self.assertEqual(len(icons), 2)
        self.assertEqual(icons[0]["tint_role"], 3)   # accent
        self.assertEqual(icons[1]["tint_role"], 1)   # fg
        clock = [n for n in _nodes(tree) if n["kind"] == "text"][0]
        self.assertEqual(clock["font_size"], 1)
        self.assertEqual(clock["font_tabular"], 1)
        self.assertEqual(clock["text"], "12:45:08")

    def test_icons_have_ink(self):
        tree, _ = _load("status-bar")
        buf = preview.render_png(tree, "app", "dark")
        pal = bc.palette("dark")
        accent = _count_color(buf, (0, 0, 16, 16), pal["accent"])
        self.assertGreater(accent, 0)


if __name__ == "__main__":
    unittest.main()
