"""blkhtml 单元/回归测试（unittest，标准库）。

覆盖：
- 40 字节记录布局与 pad0
- text_off 偏移与 -1 语义、串池去重
- 贴边四角归零（flush_rule）
- UNSUPPORTED 必须非 0 退出（--allow-lossy 才降级）
- PNG 可被 zlib 解出且尺寸正确
- design 态 vs app 态像素差异存在
- fixtures 全链路：html2blk → validate → blk2bin → preview
"""

import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL_DIR = os.path.dirname(HERE)
sys.path.insert(0, TOOL_DIR)

import blkcommon  # noqa: E402
bc = blkcommon
import blk2bin  # noqa: E402
import html2blk  # noqa: E402
import preview  # noqa: E402
import validate  # noqa: E402


def mini_tree():
    root = {"id": 0, "kind": "root", "layer": "work",
            "rect": {"x": 0, "y": 0, "w": 320, "h": 240}, "children": [
                {"id": 1, "kind": "column", "layer": "work",
                 "rect": {"x": 0, "y": 0, "w": 320, "h": 240}, "children": [
                     {"id": 2, "kind": "button", "layer": "work",
                      "rect": {"x": 0, "y": 0, "w": 120, "h": 24},
                      "text": "One"},
                 ]},
            ]}
    return {"schema": "catos.blk/1.0", "root": root}


def parse_png(png):
    assert png[:8] == b"\x89PNG\r\n\x1a\n"
    pos = 8
    idat = b""
    w = h = None
    while pos < len(png):
        ln = struct.unpack(">I", png[pos:pos + 4])[0]
        tag = png[pos + 4:pos + 8]
        data = png[pos + 8:pos + 8 + ln]
        if tag == b"IHDR":
            w, h = struct.unpack(">II", data[:8])
        elif tag == b"IDAT":
            idat += data
        pos += 12 + ln
    raw = zlib.decompress(idat)
    return w, h, raw


class TestRecordLayout(unittest.TestCase):
    def test_record_is_40_bytes_pad0_zero(self):
        node = {"id": 5, "kind": "button",
                "ce": {"id": 5, "parent": 1, "kind": 12, "layer": 200,
                       "req_w": -1, "req_h": -1, "text_off": -1,
                       "pix_off": -1, "pix_w": 0, "pix_h": 0, "res": -1},
                "rect": {"x": 0, "y": 0, "w": 120, "h": 24}}
        rec = blk2bin.serialize_rec(node, -1)
        self.assertEqual(len(rec), blkcommon.REC_BYTES)
        self.assertEqual(blkcommon.REC_BYTES, 40)
        self.assertEqual(rec[9], 0)  # pad0
        (rid, parent, kind, pad0, layer, req_w, req_h,
         text_off, pix_off, pix_w, pix_h, res) = struct.unpack(
            blkcommon.REC_FMT, rec)
        self.assertEqual((rid, parent, kind, pad0, layer), (5, 1, 12, 0, 200))
        self.assertEqual((text_off, pix_off, pix_w, pix_h, res),
                         (-1, -1, 0, 0, -1))

    def test_pool_dedupe_and_offsets(self):
        n1 = {"id": 1, "text": "Hi"}
        n2 = {"id": 2, "text": "Hi"}
        n3 = {"id": 3, "text": "Bye"}
        pool, offs = blk2bin.build_pool([n1, n2, n3])
        self.assertEqual(offs[1], offs[2])
        self.assertNotEqual(offs[1], offs[3])
        self.assertEqual(pool, b"Hi\0Bye\0")
        blk2bin.verify_pool(pool, offs, [n1, n2, n3])

    def test_text_off_minus_one_when_no_text(self):
        n = {"id": 1, "kind": "rect"}
        pool, offs = blk2bin.build_pool([n])
        self.assertEqual(offs[1], -1)


class TestFlushRule(unittest.TestCase):
    def test_touching_sides_zero_corners_and_stroke(self):
        parent = {"id": 1, "kind": "column", "rect": {"x": 0, "y": 0,
                 "w": 160, "h": 80}, "padding": 0, "children": []}
        child = {"id": 2, "kind": "rect", "rect": {"x": 0, "y": 0,
                 "w": 80, "h": 40},
                 "radius": {"tl": 12, "tr": 12, "br": 12, "bl": 12},
                 "stroke": {"w": 1, "sides": ["L", "R", "T", "B"],
                            "color": "border"},
                 "shadow": {"mode": "contact", "sides": ["L", "R", "T", "B"]}}
        parent["children"].append(child)
        html2blk.apply_flush(parent, html2blk.Diag())
        # 子块贴父内容框 L 与 T
        self.assertIn("L", child["flush_sides"])
        self.assertIn("T", child["flush_sides"])
        self.assertEqual(child["radius"]["tl"], 0)
        self.assertEqual(child["radius"]["bl"], 0)
        self.assertEqual(child["radius"]["tr"], 0)  # T 侧归零
        self.assertNotIn("L", child["stroke"]["sides"])
        self.assertNotIn("T", child["stroke"]["sides"])
        self.assertNotIn("L", child["shadow"]["sides"])
        self.assertNotIn("T", child["shadow"]["sides"])


class TestUnsupportedExit(unittest.TestCase):
    def _run(self, html, lossy):
        with tempfile.TemporaryDirectory() as d:
            src = os.path.join(d, "in.html")
            out = os.path.join(d, "out.blk.json")
            with open(src, "w", encoding="utf-8") as f:
                f.write(html)
            argv = [src, "--out", out]
            if lossy:
                argv.append("--allow-lossy")
            return html2blk.main(argv)

    def test_unsupported_exits_nonzero(self):
        html = "<div style=\"float:left;\">x</div>"
        self.assertEqual(self._run(html, False), 1)

    def test_lossy_degrades_to_warning(self):
        html = "<div style=\"float:left;\">x</div>"
        self.assertEqual(self._run(html, True), 0)


class TestPng(unittest.TestCase):
    def test_png_decodes_and_size_matches(self):
        tree = mini_tree()
        buf = preview.render_png(tree, "app", "dark")
        png = preview.encode_png(buf.w, buf.h, buf.rows)
        w, h, raw = parse_png(png)
        self.assertEqual((w, h), (buf.w, buf.h))
        self.assertEqual(len(raw), h * (1 + w * 3))

    def test_design_differs_from_app(self):
        tree = mini_tree()
        app = preview.render_png(tree, "app", "dark")
        design = preview.render_png(tree, "design", "dark")
        self.assertEqual((app.w, app.h), (design.w, design.h))
        diff = any(app.rows[i] != design.rows[i]
                   for i in range(app.h))
        self.assertTrue(diff, "design 与 app 像素应存在差异")


class TestFixturePipeline(unittest.TestCase):
    def test_fixture_end_to_end(self):
        fixture = os.path.join(HERE, "fixtures", "sample.html")
        with open(fixture, "r", encoding="utf-8") as f:
            html_text = f.read()
        tree, diag = html2blk.convert(html_text, "fixture", "dark")
        self.assertFalse(diag.hard_fail, diag.lines())

        rep = validate.Report()
        validate.validate_tree(tree, rep)
        self.assertFalse(rep.failed, rep.text_lines())

        nodes = blk2bin.collect_nodes(tree["root"])
        pool, offs = blk2bin.build_pool(nodes)
        blk2bin.verify_pool(pool, offs, nodes)
        recs = b"".join(blk2bin.serialize_rec(n, offs[n["id"]]) for n in nodes)
        self.assertEqual(len(recs), len(nodes) * 40)
        self.assertEqual(len(recs), 40 * len(nodes))
        first = struct.unpack(blkcommon.REC_FMT, recs[:40])
        self.assertEqual(first[8], -1)  # pix_off 恒 -1

        buf = preview.render_png(tree, "app", "dark")
        png = preview.encode_png(buf.w, buf.h, buf.rows)
        w, h, raw = parse_png(png)
        self.assertEqual((w, h), (800, 600))


class TestFixB1FreeRecursion(unittest.TestCase):
    """B1：FREE 父块落位后必须递归后代，子树不能停在 (0,0)。"""

    def test_free_subtree_coordinates(self):
        html = """
        <div style="position:absolute;width:320px;height:240px;">
          <div style="position:absolute;left:16px;top:24px;width:120px;height:80px;">
            <div style="position:absolute;left:8px;top:8px;width:40px;height:24px;"></div>
          </div>
        </div>"""
        tree, diag = html2blk.convert(html, "t", "dark")
        self.assertFalse(diag.hard_fail, diag.lines())
        outer = tree["root"]["children"][0]
        inner = outer["children"][0]
        grand = inner["children"][0]
        # 内层内容框原点 (16,24)；孙块相对它 left:8 top:8 → (24,32)。
        self.assertEqual(inner["rect"]["x"], 16)
        self.assertEqual(inner["rect"]["y"], 24)
        self.assertEqual(grand["rect"]["x"], 24)
        self.assertEqual(grand["rect"]["y"], 32)
        self.assertNotEqual((grand["rect"]["x"], grand["rect"]["y"]), (0, 0))


class TestFixB2GapPx(unittest.TestCase):
    """B2：gap 接受 N 与 Npx；档位仍须属 allowed_spacing。"""

    def test_gap_4px_parsed(self):
        html = """
        <div style="display:flex;flex-direction:row;gap:4px;">
          <button>A</button><button>B</button>
        </div>"""
        tree, diag = html2blk.convert(html, "t", "dark")
        self.assertFalse(diag.hard_fail, diag.lines())
        row = tree["root"]["children"][0]
        self.assertEqual(row["layout"]["gap"], 4)

    def test_gap_bad_ladder_still_unsupported(self):
        html = '<div style="gap:3px;"><span>x</span></div>'
        tree, diag = html2blk.convert(html, "t", "dark")
        self.assertTrue(diag.hard_fail)


class TestFixB3StrokeRole(unittest.TestCase):
    """B3：stroke.color 角色名走调色板解析，hex 原样返回。"""

    def test_role_name_resolved(self):
        node = {"stroke": {"w": 1, "sides": ["L"], "color": "border"}}
        self.assertEqual(preview._stroke_color(node, "dark"),
                         bc.palette("dark")["border"])

    def test_hex_passthrough(self):
        node = {"stroke": {"w": 1, "sides": ["L"], "color": "#181820"}}
        self.assertEqual(preview._stroke_color(node, "dark"), "181820")

    def test_render_no_role_value_error(self):
        tree = {"schema": "catos.blk/1.0", "root": {
            "id": 0, "kind": "root", "layer": "work",
            "rect": {"x": 0, "y": 0, "w": 160, "h": 80}, "children": [
                {"id": 1, "kind": "rect", "layer": "work",
                 "rect": {"x": 0, "y": 0, "w": 160, "h": 80},
                 "stroke": {"w": 1, "sides": ["L", "R", "T", "B"],
                            "color": "border"}}]}}
        buf = preview.render_png(tree, "app", "dark")
        self.assertEqual((buf.w, buf.h), (160, 80))


class TestFixB4ScrollWarn(unittest.TestCase):
    """B4：scroll/visible 越界只 WARN；clip 才 FAIL；B2 越外框仍 FAIL。"""

    def _tree(self, overflow):
        return {"schema": "catos.blk/1.0", "root": {
            "id": 0, "kind": "root", "layer": "work",
            "rect": {"x": 0, "y": 0, "w": 160, "h": 80}, "children": [
                {"id": 1, "kind": "column", "layer": "work",
                 "rect": {"x": 0, "y": 0, "w": 160, "h": 80},
                 "padding": {"t": 8, "r": 8, "b": 8, "l": 8},
                 "overflow": overflow, "children": [
                     {"id": 2, "kind": "rect", "layer": "work",
                      "rect": {"x": 120, "y": 56, "w": 40, "h": 24}}]}]}}

    def test_scroll_outside_content_warns_not_fails(self):
        rep = validate.Report()
        validate.validate_tree(self._tree("scroll"), rep)
        b1 = [r for r in rep.results if r["rule"] == "B1"]
        self.assertTrue(any(r["status"] == "WARN" for r in b1))
        self.assertFalse(rep.failed)

    def test_clip_outside_content_fails(self):
        rep = validate.Report()
        validate.validate_tree(self._tree("clip"), rep)
        self.assertTrue(rep.failed)
        self.assertTrue(any(r["rule"] == "B1" and r["status"] == "FAIL"
                             for r in rep.results))


class TestFixB5VoidElements(unittest.TestCase):
    """B5：input/hr/img/br 自动闭合，后续兄弟挂到父块而非 void 元素。"""

    def test_input_then_sibling_parent(self):
        html = """
        <div>
          <input type="text" placeholder="addr"/>
          <span>sibling</span>
        </div>"""
        tree, diag = html2blk.convert(html, "t", "dark")
        self.assertFalse(diag.hard_fail, diag.lines())
        div = tree["root"]["children"][0]
        self.assertEqual(len(div["children"]), 2)
        inp, span = div["children"]
        self.assertEqual(inp["kind"], "input")
        self.assertEqual(inp.get("children", []), [])
        self.assertEqual(span["kind"], "text")
        self.assertEqual(span["ce"]["parent"], div["id"])
        self.assertEqual(inp["ce"]["parent"], div["id"])


class TestFixB6RoleColorEnum(unittest.TestCase):
    """B6：emit-c 角色名 → role_* 枚举；只有字面 RGB 才固化进 fg/bg/accent。"""

    def test_role_maps_to_role_enum(self):
        self.assertEqual(blk2bin.color_expr("bar"),
                         ("INHERIT_COLOR", "ROLE_BAR"))
        self.assertEqual(blk2bin.color_expr("panel"),
                         ("INHERIT_COLOR", "ROLE_PANEL"))
        self.assertEqual(blk2bin.color_expr("bg"),
                         ("INHERIT_COLOR", "ROLE_BG"))
        self.assertEqual(blk2bin.color_expr("fg"),
                         ("INHERIT_COLOR", "ROLE_FG"))
        self.assertEqual(blk2bin.color_expr("accent"),
                         ("INHERIT_COLOR", "ROLE_ACCENT"))

    def test_panel_role_emitted(self):
        html = '<div style="background:panel;width:320px;height:240px;"></div>'
        tree, diag = html2blk.convert(html, "t", "dark")
        self.assertFalse(diag.hard_fail, diag.lines())
        nodes = list(bc.iter_nodes(tree["root"]))
        pool, _ = blk2bin.build_pool(nodes)
        src = blk2bin.emit_c(tree, nodes, pool)
        self.assertIn(".role_bg = ROLE_PANEL", src)
        self.assertIn(".bg = INHERIT_COLOR", src)

    def test_hex_burned_into_color(self):
        self.assertEqual(blk2bin.color_expr("#181820"),
                         ("0x181820u", "ROLE_NONE"))
        self.assertEqual(blk2bin.color_expr("181820"),
                         ("0x181820u", "ROLE_NONE"))

    def test_emit_c_uses_role_not_string_literal(self):
        html = """
        <div style="background:bar;width:320px;height:240px;">
          <button>A</button>
        </div>"""
        tree, diag = html2blk.convert(html, "t", "dark")
        self.assertFalse(diag.hard_fail, diag.lines())
        nodes = list(bc.iter_nodes(tree["root"]))
        pool, _ = blk2bin.build_pool(nodes)
        src = blk2bin.emit_c(tree, nodes, pool)
        # 角色名不能写成 C 字符串字面量（.bg = "bar" 编不过）。
        self.assertNotIn('"bar"', src)
        self.assertIn(".role_bg = ROLE_BAR", src)
        self.assertIn(".bg = INHERIT_COLOR", src)


class TestFixB7KindMatchesFlow(unittest.TestCase):
    """B7：容器 kind 由 flex-direction 决定；emit-c 同口径映射 kind 常量。"""

    def test_row_div_kind_is_row(self):
        html = '<div style="display:flex;flex-direction:row;"><button>A</button></div>'
        tree, diag = html2blk.convert(html, "t", "dark")
        self.assertFalse(diag.hard_fail, diag.lines())
        row = tree["root"]["children"][0]
        self.assertEqual(row["kind"], "row")
        nodes = list(bc.iter_nodes(tree["root"]))
        pool, _ = blk2bin.build_pool(nodes)
        src = blk2bin.emit_c(tree, nodes, pool)
        # BLK_ROW == 2（blkcommon.KIND_FROM_NAME）。
        self.assertIn(".kind = BLK_ROW", src)

    def test_column_div_kind_is_column(self):
        html = '<div style="display:flex;flex-direction:column;"><button>A</button></div>'
        tree, diag = html2blk.convert(html, "t", "dark")
        col = tree["root"]["children"][0]
        self.assertEqual(col["kind"], "column")


class TestFixCEKindWhitelist(unittest.TestCase):
    """--ce：button/input/term 不在 COLUMN..CANVAS 区间，必须 FAIL；默认口径不收影响。"""

    def test_button_rejected_under_ce(self):
        rep = validate.Report()
        validate.validate_tree(mini_tree(), rep, ce_projection=True)
        self.assertTrue(any(
            r["rule"] == "L1" and r["status"] == "FAIL"
            and "kind=button" in r["detail"] for r in rep.results))

    def test_plain_mode_accepts_button(self):
        rep = validate.Report()
        validate.validate_tree(mini_tree(), rep, ce_projection=False)
        self.assertFalse(rep.failed)


# 内核 include 目录：优先环境变量 CATOS_KERNEL_INC，否则走 blkcommon 同一套
# （CATOS_ROOT → 仓库内相对 → 兄弟树兜底）解析，不再写死绝对路径。
KERNEL_INC = os.environ.get("CATOS_KERNEL_INC") or bc.kernel_inc_dir()


class TestLayoutAlignment(unittest.TestCase):
    """P2：row/column 主轴与交叉轴对齐，口径照抄 kernel layout.c。"""

    def test_row_space_between(self):
        # 反例 1：settings 标题栏。父 784×40、pad 8；左文本 x=8，
        # 右按钮右缘贴内容框右 776（x=712, w=64）。
        html = """
        <div style="display:flex;flex-direction:row;justify-content:space-between;padding:8px;width:784px;height:40px;">
          <span>Settings</span>
          <button style="width:64px;height:24px;">Close</button>
        </div>"""
        tree, diag = html2blk.convert(html, "t", "dark")
        self.assertFalse(diag.hard_fail, diag.lines())
        row = tree["root"]["children"][0]
        text, btn = row["children"]
        self.assertEqual(text["rect"], {"x": 8, "y": 8, "w": 64, "h": 16})
        self.assertEqual(btn["rect"], {"x": 712, "y": 8, "w": 64, "h": 24})

    def test_row_align_items_center(self):
        # 反例 2：toolbar。父高 48、子高 24，交叉轴居中 → y=(48-24)/2=12。
        html = """
        <div style="display:flex;flex-direction:row;align-items:center;width:480px;height:48px;">
          <button style="width:48px;height:24px;">Back</button>
        </div>"""
        tree, diag = html2blk.convert(html, "t", "dark")
        self.assertFalse(diag.hard_fail, diag.lines())
        row = tree["root"]["children"][0]
        btn = row["children"][0]
        self.assertEqual(btn["rect"]["y"], 12)


class TestEmitCCompileSmoke(unittest.TestCase):
    """B6 收口：emit-c 片段必须能被 gcc -m32 -ffreestanding 编到 .o。"""

    def test_sample_c_compiles_to_object(self):
        if not shutil.which("gcc") or not os.path.isdir(KERNEL_INC):
            self.skipTest("gcc 或内核 include 不可用")
        tree = mini_tree()
        nodes = list(bc.iter_nodes(tree["root"]))
        pool, _ = blk2bin.build_pool(nodes)
        src = blk2bin.emit_c(tree, nodes, pool)
        with tempfile.TemporaryDirectory() as d:
            c_path = os.path.join(d, "sample.c")
            o_path = os.path.join(d, "sample.o")
            with open(c_path, "w", encoding="utf-8") as f:
                f.write(src)
            r = subprocess.run(
                ["gcc", "-m32", "-ffreestanding", "-Wall", "-Wextra",
                 "-I" + KERNEL_INC, "-c", c_path, "-o", o_path],
                capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)


def _make_fake_repo(files):
    d = tempfile.mkdtemp()
    for rel, content in files.items():
        p = os.path.join(d, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w", encoding="utf-8") as f:
            f.write(content)
    return d


DECL_C = "int validate(void) {\n    return 0;\n}\n"
TOKENS_H = "#define FOO_ROLE 1\nstatic const int foo = 3;\n"


class TestSpecAudit(unittest.TestCase):
    """--spec-audit：规范 md 引用 ↔ 内核源码一致性断言（A-9-01..04）。"""

    def _run(self, md, files):
        d = _make_fake_repo(files)
        md_path = os.path.join(d, "spec.md")
        with open(md_path, "w", encoding="utf-8") as f:
            f.write(md)
        results, _ = validate.run_spec_audit(md_path, repo_root=d)
        rc = validate.main(["--spec-audit", "--md", md_path], repo_root=d)
        return results, rc

    def test_all_pass_with_correct_refs(self):
        md = ("# 规范草稿\n\n"
              "引用 `kernel_v25/src/gui/decl.c::validate()`。\n\n"
              "内置页字段见 `blk_spec.anchor`。\n")
        results, rc = self._run(
            md, {"kernel_v25/src/gui/decl.c": DECL_C,
                 "kernel_v25/include/gui/tokens.h": TOKENS_H})
        fails = [r for r in results if r["status"] == "FAIL"]
        self.assertEqual(fails, [])
        self.assertEqual(rc, 0)
        self.assertTrue(any(r["rule"] == "A-9-02" and r["status"] == "PASS"
                            for r in results))

    def test_missing_symbol_rc1(self):
        md = ("引用 `kernel_v25/src/gui/decl.c::no_such_symbol()`。\n\n"
              "`blk_spec.anchor`\n")
        results, rc = self._run(
            md, {"kernel_v25/src/gui/decl.c": DECL_C,
                 "kernel_v25/include/gui/tokens.h": TOKENS_H})
        self.assertEqual(rc, 1)
        self.assertTrue(any(r["rule"] == "A-9-02" and r["status"] == "FAIL"
                            and r["symbol"] == "no_such_symbol"
                            for r in results))

    def test_misfiled_symbol_rc1(self):
        # foo 只在 tokens.h 出现，文档却引 decl.c::foo → A-9-03 跨文件误引。
        md = ("引用 `kernel_v25/src/gui/decl.c::foo()`。\n\n"
              "`blk_spec.anchor`\n")
        results, rc = self._run(
            md, {"kernel_v25/src/gui/decl.c": DECL_C,
                 "kernel_v25/include/gui/tokens.h": TOKENS_H})
        self.assertEqual(rc, 1)
        self.assertTrue(any(r["rule"] == "A-9-03" and r["status"] == "FAIL"
                            and "tokens.h" in r["detail"]
                            for r in results))

    def test_drifted_line_hint_warns_not_fails(self):
        # 符号实际在第 2 行；行号提示 L999 漂移 → WARN，不判 FAIL，RC=0。
        md = ("引用 `kernel_v25/src/gui/decl.c::validate() (L999)`。\n\n"
              "`blk_spec.anchor`\n")
        results, rc = self._run(md, {"kernel_v25/src/gui/decl.c": DECL_C})
        self.assertEqual(rc, 0)
        self.assertFalse(any(r["status"] == "FAIL" for r in results))
        self.assertTrue(any(r["status"] == "WARN"
                            and "line hint drifted" in r["detail"]
                            for r in results))

    def test_audit_results_carry_rule_field(self):
        md = "引用 `kernel_v25/src/gui/decl.c::validate()`。\n`blk_spec.anchor`\n"
        results, _ = self._run(md, {"kernel_v25/src/gui/decl.c": DECL_C})
        for r in results:
            self.assertIn("rule", r)


class TestRuleIdPrefix(unittest.TestCase):
    """诊断行首统一带 [规则ID] 前缀；--json 每条带 rule 字段。"""

    def test_json_results_carry_rule_field(self):
        rep = validate.Report()
        validate.validate_tree(mini_tree(), rep)
        payload = json.loads(json.dumps({"results": rep.results}))
        self.assertTrue(payload["results"])
        for r in payload["results"]:
            self.assertIn("rule", r)

    def test_fail_line_starts_with_rule_prefix(self):
        tree = {"schema": "catos.blk/1.0", "root": {
            "id": 0, "kind": "root", "layer": "work",
            "rect": {"x": 0, "y": 0, "w": 160, "h": 80}, "children": [
                {"id": 1, "kind": "rect", "layer": "work",
                 "rect": {"x": 0, "y": 0, "w": 12, "h": 24}}]}}
        rep = validate.Report()
        validate.validate_tree(tree, rep)
        g1 = [l for l in rep.text_lines() if "[G1]" in l]
        self.assertTrue(g1)
        self.assertTrue(any(l.startswith("[G1] FAIL") for l in g1))


class TestCap2Alpha(unittest.TestCase):
    """M-CAP2-01/02：data-alpha 比例 / 绝对值判别 + alpha_mode。"""

    def _conv(self, html):
        return html2blk.convert(html, "t", "dark")

    def test_ratio_half_maps_to_128(self):
        tree, diag = self._conv('<span data-alpha="0.5">x</span>')
        self.assertFalse(diag.hard_fail, diag.lines())
        node = tree["root"]["children"][0]
        self.assertEqual(node["alpha"], 128)

    def test_integer_one_is_ratio_255(self):
        tree, diag = self._conv('<span data-alpha="1">x</span>')
        self.assertFalse(diag.hard_fail, diag.lines())
        self.assertEqual(tree["root"]["children"][0]["alpha"], 255)

    def test_absolute_200_passthrough(self):
        tree, diag = self._conv('<span data-alpha="200">x</span>')
        self.assertFalse(diag.hard_fail, diag.lines())
        self.assertEqual(tree["root"]["children"][0]["alpha"], 200)

    def test_zero_means_undeclared(self):
        tree, diag = self._conv('<span data-alpha="0">x</span>')
        self.assertFalse(diag.hard_fail, diag.lines())
        self.assertEqual(tree["root"]["children"][0]["alpha"], 0)

    def test_ratio_over_one_rejected(self):
        tree, diag = self._conv('<span data-alpha="1.5">x</span>')
        self.assertTrue(diag.hard_fail)
        self.assertTrue(any(i["prop"] == "M-CAP2-01" for i in diag.items))

    def test_absolute_over_255_rejected(self):
        tree, diag = self._conv('<span data-alpha="300">x</span>')
        self.assertTrue(diag.hard_fail)
        self.assertTrue(any(i["prop"] == "M-CAP2-01" for i in diag.items))

    def test_alpha_mode_map(self):
        tree, diag = self._conv(
            '<span data-alpha-mode="none">x</span>')
        self.assertFalse(diag.hard_fail, diag.lines())
        self.assertEqual(tree["root"]["children"][0]["alpha_mode"], 1)
        tree, diag = self._conv(
            '<span data-alpha-mode="ordered16">x</span>')
        self.assertEqual(tree["root"]["children"][0]["alpha_mode"], 0)

    def test_alpha_mode_bad_diagnosed(self):
        tree, diag = self._conv('<span data-alpha-mode="weird">x</span>')
        self.assertTrue(diag.hard_fail)
        self.assertTrue(any(i["prop"] == "M-CAP2-02" for i in diag.items))


class TestCap2ElevationRadius(unittest.TestCase):
    """M-CAP2-03/04/05：elevation 档位与逐角半径哨兵。"""

    def test_elevation_names_and_numbers(self):
        cases = {"flat": 0, "raised": 1, "hover": 2, "float": 3,
                 "overlay": 4, "modal": 5}
        for name, val in cases.items():
            tree, diag = html2blk.convert(
                '<span data-elevation="%s">x</span>' % name, "t", "dark")
            self.assertFalse(diag.hard_fail, diag.lines())
            self.assertEqual(tree["root"]["children"][0]["elevation"], val,
                             name)

    def test_elevation_bad_diagnosed(self):
        tree, diag = html2blk.convert(
            '<span data-elevation="sideways">x</span>', "t", "dark")
        self.assertTrue(diag.hard_fail)
        self.assertTrue(any(i["prop"] == "M-CAP2-03" for i in diag.items))

    def _node(self, attrs):
        el = html2blk.El("span", dict(attrs))
        node = {"kind": "text"}
        diag = html2blk.Diag()
        html2blk.apply_cap2(el, node, diag, "s")
        return node, diag

    def test_radius_single_value_all_corners(self):
        node, diag = self._node({"data-radius": "16"})
        self.assertFalse(diag.hard_fail, diag.lines())
        self.assertEqual(node["radius"],
                         {"tl": 16, "tr": 16, "br": 16, "bl": 16})

    def test_radius_four_values_tl_tr_br_bl(self):
        node, diag = self._node({"data-radius": "16 16 0 0"})
        self.assertFalse(diag.hard_fail, diag.lines())
        self.assertEqual(node["radius"],
                         {"tl": 16, "tr": 16, "br": 0, "bl": 0})

    def test_radius_sentinels(self):
        for tok, val in (("pill", 128), ("auto", 255), ("inherit", 0)):
            node, diag = self._node({"data-radius": tok})
            self.assertFalse(diag.hard_fail, diag.lines())
            self.assertEqual(node["radius"]["tl"], val, tok)

    def test_radius_per_corner_override(self):
        node, diag = self._node({"data-radius": "8",
                                  "data-radius-tl": "pill",
                                  "data-radius-br": "auto"})
        self.assertFalse(diag.hard_fail, diag.lines())
        rad = node["radius"]
        self.assertEqual(rad["tl"], 128)
        self.assertEqual(rad["br"], 255)
        self.assertEqual(rad["tr"], 8)
        self.assertEqual(rad["bl"], 8)

    def test_radius_px_over_63_rejected(self):
        node, diag = self._node({"data-radius": "99"})
        self.assertTrue(diag.hard_fail)
        self.assertTrue(any(i["prop"] == "M-CAP2-04" for i in diag.items))

    def test_radius_corner_bad_diagnosed(self):
        node, diag = self._node({"data-radius-tr": "99"})
        self.assertTrue(diag.hard_fail)
        self.assertTrue(any(i["prop"] == "M-CAP2-05" for i in diag.items))


class TestCap2Text(unittest.TestCase):
    """M-CAP2-07..12：折行 / 省略 / 字号 / 粗体 / 等宽 / 行高。"""

    def test_wrap_values(self):
        for v, exp in (("0", 0), ("3", 3), ("6", 6), ("auto", 255)):
            tree, diag = html2blk.convert(
                '<span data-wrap="%s">x</span>' % v, "t", "dark")
            self.assertFalse(diag.hard_fail, diag.lines())
            self.assertEqual(tree["root"]["children"][0]["wrap_lines"], exp, v)

    def test_wrap_bad_diagnosed(self):
        tree, diag = html2blk.convert(
            '<span data-wrap="9">x</span>', "t", "dark")
        self.assertTrue(diag.hard_fail)
        self.assertTrue(any(i["prop"] == "M-CAP2-07" for i in diag.items))

    def test_ellipsis_map(self):
        for v, exp in (("none", 0), ("char", 1), ("word", 2)):
            tree, diag = html2blk.convert(
                '<span data-ellipsis="%s">x</span>' % v, "t", "dark")
            self.assertFalse(diag.hard_fail, diag.lines())
            self.assertEqual(tree["root"]["children"][0]["ellipsis"], exp, v)

    def test_ellipsis_bad_diagnosed(self):
        tree, diag = html2blk.convert(
            '<span data-ellipsis="middle">x</span>', "t", "dark")
        self.assertTrue(diag.hard_fail)
        self.assertTrue(any(i["prop"] == "M-CAP2-08" for i in diag.items))

    def test_font_size_map(self):
        cases = {"8": 1, "caption": 1, "16": 2, "body": 2,
                 "24": 3, "title": 3, "32": 4, "display": 4}
        for v, exp in cases.items():
            tree, diag = html2blk.convert(
                '<span data-font="%s">x</span>' % v, "t", "dark")
            self.assertFalse(diag.hard_fail, diag.lines())
            self.assertEqual(tree["root"]["children"][0]["font_size"], exp, v)

    def test_font_bad_diagnosed(self):
        tree, diag = html2blk.convert(
            '<span data-font="99">x</span>', "t", "dark")
        self.assertTrue(diag.hard_fail)
        self.assertTrue(any(i["prop"] == "M-CAP2-09" for i in diag.items))

    def test_bold_tabular_bool(self):
        tree, diag = html2blk.convert(
            '<span data-bold="true" data-tabular="0">x</span>', "t", "dark")
        self.assertFalse(diag.hard_fail, diag.lines())
        node = tree["root"]["children"][0]
        self.assertEqual(node["font_bold"], 1)
        self.assertEqual(node["font_tabular"], 0)

    def test_bold_bad_diagnosed(self):
        tree, diag = html2blk.convert(
            '<span data-bold="maybe">x</span>', "t", "dark")
        self.assertTrue(diag.hard_fail)
        self.assertTrue(any(i["prop"] == "M-CAP2-10" for i in diag.items))

    def test_line_h(self):
        tree, diag = html2blk.convert(
            '<span data-line-h="24">x</span>', "t", "dark")
        self.assertFalse(diag.hard_fail, diag.lines())
        self.assertEqual(tree["root"]["children"][0]["line_h"], 24)

    def test_line_h_over_255_rejected(self):
        tree, diag = html2blk.convert(
            '<span data-line-h="300">x</span>', "t", "dark")
        self.assertTrue(diag.hard_fail)
        self.assertTrue(any(i["prop"] == "M-CAP2-12" for i in diag.items))


class TestCap2Anim(unittest.TestCase):
    """M-CAP2-13/14/15：动效时长 / 缓动 / 属性掩码。"""

    def test_anim_ms_ladder(self):
        for v in (48, 96, 144, 192, 288):
            tree, diag = html2blk.convert(
                '<span data-anim="%d">x</span>' % v, "t", "dark")
            self.assertFalse(diag.hard_fail, diag.lines())
            self.assertEqual(tree["root"]["children"][0]["anim_ms"], v)

    def test_anim_ms_bad_diagnosed(self):
        tree, diag = html2blk.convert(
            '<span data-anim="120">x</span>', "t", "dark")
        self.assertTrue(diag.hard_fail)
        self.assertTrue(any(i["prop"] == "M-CAP2-13" for i in diag.items))

    def test_ease_map(self):
        cases = {"none": 0, "linear": 1, "in": 2, "out": 3, "inout": 4,
                 "out-back": 5, "in-back": 6, "step": 7}
        for v, exp in cases.items():
            tree, diag = html2blk.convert(
                '<span data-ease="%s">x</span>' % v, "t", "dark")
            self.assertFalse(diag.hard_fail, diag.lines())
            self.assertEqual(tree["root"]["children"][0]["anim_ease"], exp, v)

    def test_ease_bad_diagnosed(self):
        tree, diag = html2blk.convert(
            '<span data-ease="bounce">x</span>', "t", "dark")
        self.assertTrue(diag.hard_fail)
        self.assertTrue(any(i["prop"] == "M-CAP2-14" for i in diag.items))

    def test_anim_props_mask(self):
        tree, diag = html2blk.convert(
            '<span data-anim-props="x,alpha">x</span>', "t", "dark")
        self.assertFalse(diag.hard_fail, diag.lines())
        self.assertEqual(tree["root"]["children"][0]["anim_mask"], 1 << 0 | 1 << 4)

    def test_anim_props_bad_diagnosed(self):
        tree, diag = html2blk.convert(
            '<span data-anim-props="z">x</span>', "t", "dark")
        self.assertTrue(diag.hard_fail)
        self.assertTrue(any(i["prop"] == "M-CAP2-15" for i in diag.items))


class TestCap2PopoverIcon(unittest.TestCase):
    """M-CAP2-16..20：浮层锚定 / 朝向 / 限高 / 图标 / 着色。"""

    def test_popover_kind_and_prefer_maxh(self):
        html = ('<button id="btn1">OK</button>'
                '<c-popover data-anchor="btn1" data-prefer="down|right" '
                'data-max-h="120"><span>menu</span></c-popover>')
        tree, diag = html2blk.convert(html, "t", "dark")
        self.assertFalse(diag.hard_fail, diag.lines())
        pop = [n for n in bc.iter_nodes(tree["root"])
               if n["kind"] == "popover"][0]
        self.assertEqual(pop["anchor_id"], 1)
        self.assertEqual(pop["pop_prefer"], 2 | 8)
        self.assertEqual(pop["pop_max_h"], 120)

    def test_popover_anchor_unresolved_diagnosed(self):
        html = '<c-popover data-anchor="nope"><span>m</span></c-popover>'
        tree, diag = html2blk.convert(html, "t", "dark")
        self.assertTrue(diag.hard_fail)
        self.assertTrue(any(i["prop"] == "M-CAP2-16" for i in diag.items))

    def test_prefer_bad_diagnosed(self):
        tree, diag = html2blk.convert(
            '<c-popover data-prefer="upleft"><span>m</span></c-popover>',
            "t", "dark")
        self.assertTrue(diag.hard_fail)
        self.assertTrue(any(i["prop"] == "M-CAP2-17" for i in diag.items))

    def test_max_h_over_u16_rejected(self):
        tree, diag = html2blk.convert(
            '<c-popover data-max-h="70000"><span>m</span></c-popover>',
            "t", "dark")
        self.assertTrue(diag.hard_fail)
        self.assertTrue(any(i["prop"] == "M-CAP2-18" for i in diag.items))

    def test_data_icon_switches_kind(self):
        tree, diag = html2blk.convert(
            '<span data-icon="7" data-tint="accent"></span>', "t", "dark")
        self.assertFalse(diag.hard_fail, diag.lines())
        node = tree["root"]["children"][0]
        self.assertEqual(node["kind"], "icon")
        self.assertEqual(node["icon"], 7)
        self.assertEqual(node["tint_role"], 3)

    def test_c_icon_tag(self):
        tree, diag = html2blk.convert('<c-icon data-icon="2"></c-icon>',
                                      "t", "dark")
        self.assertFalse(diag.hard_fail, diag.lines())
        node = tree["root"]["children"][0]
        self.assertEqual(node["kind"], "icon")
        self.assertEqual(node["icon"], 2)

    def test_icon_number_over_255_rejected(self):
        tree, diag = html2blk.convert(
            '<span data-icon="300"></span>', "t", "dark")
        self.assertTrue(diag.hard_fail)
        self.assertTrue(any(i["prop"] == "M-CAP2-19" for i in diag.items))

    def test_tint_map_and_bad(self):
        for v, exp in (("fg", 1), ("bg", 2), ("accent", 3), ("border", 4)):
            tree, diag = html2blk.convert(
                '<c-icon data-icon="1" data-tint="%s"></c-icon>' % v,
                "t", "dark")
            self.assertFalse(diag.hard_fail, diag.lines())
            self.assertEqual(tree["root"]["children"][0]["tint_role"], exp, v)
        tree, diag = html2blk.convert(
            '<c-icon data-icon="1" data-tint="red"></c-icon>', "t", "dark")
        self.assertTrue(diag.hard_fail)
        self.assertTrue(any(i["prop"] == "M-CAP2-20" for i in diag.items))


class TestCap2ScrollTheme(unittest.TestCase):
    """M-CAP2-06/21/22/23：描边侧别 / 滚动条 / 滚动轴 / HC 覆盖。"""

    def test_border_sides_mask(self):
        tree, diag = html2blk.convert(
            '<div data-border-sides="l,t"></div>', "t", "dark")
        self.assertFalse(diag.hard_fail, diag.lines())
        self.assertEqual(tree["root"]["children"][0]["border_sides"],
                         1 << 0 | 1 << 2)

    def test_border_sides_bad_diagnosed(self):
        tree, diag = html2blk.convert(
            '<div data-border-sides="x"></div>', "t", "dark")
        self.assertTrue(diag.hard_fail)
        self.assertTrue(any(i["prop"] == "M-CAP2-06" for i in diag.items))

    def test_scrollbar_map(self):
        for v, exp in (("auto", 0), ("always", 1), ("never", 2)):
            tree, diag = html2blk.convert(
                '<div data-scrollbar="%s"></div>' % v, "t", "dark")
            self.assertFalse(diag.hard_fail, diag.lines())
            self.assertEqual(tree["root"]["children"][0]["scrollbar"], exp, v)

    def test_scroll_axis_map(self):
        for v, exp in (("x", 1), ("y", 2), ("both", 3)):
            tree, diag = html2blk.convert(
                '<div data-scroll="%s"></div>' % v, "t", "dark")
            self.assertFalse(diag.hard_fail, diag.lines())
            self.assertEqual(tree["root"]["children"][0]["scroll_axis"], exp, v)

    def test_scrollbar_bad_diagnosed(self):
        tree, diag = html2blk.convert(
            '<div data-scrollbar="auto-x"></div>', "t", "dark")
        self.assertTrue(diag.hard_fail)
        self.assertTrue(any(i["prop"] == "M-CAP2-21" for i in diag.items))

    def test_scroll_axis_bad_diagnosed(self):
        tree, diag = html2blk.convert(
            '<div data-scroll="z"></div>', "t", "dark")
        self.assertTrue(diag.hard_fail)
        self.assertTrue(any(i["prop"] == "M-CAP2-22" for i in diag.items))

    def test_theme_hc_force(self):
        tree, diag = html2blk.convert(
            '<div data-theme="hc"><span>x</span></div>', "t", "dark")
        self.assertFalse(diag.hard_fail, diag.lines())
        self.assertEqual(tree["root"]["children"][0]["hc_force"], 1)

    def test_theme_bad_diagnosed(self):
        tree, diag = html2blk.convert(
            '<div data-theme="light"></div>', "t", "dark")
        self.assertTrue(diag.hard_fail)
        self.assertTrue(any(i["prop"] == "M-CAP2-23" for i in diag.items))


class TestCap2ValidateC2(unittest.TestCase):
    """C2：cap2 字段值域校验。"""

    def _rep(self, node):
        tree = {"schema": "catos.blk/1.0", "root": {
            "id": 0, "kind": "root", "layer": "work",
            "rect": {"x": 0, "y": 0, "w": 160, "h": 80},
            "children": [node]}}
        rep = validate.Report()
        validate.validate_tree(tree, rep)
        return rep

    def test_valid_cap2_node_passes(self):
        node = {"id": 1, "kind": "column", "layer": "work",
                "rect": {"x": 0, "y": 0, "w": 160, "h": 80},
                "alpha": 128, "elevation": 3, "wrap_lines": 2,
                "ellipsis": 2, "font_size": 3, "anim_ms": 96,
                "anim_ease": 5, "anim_mask": 17, "pop_prefer": 10,
                "pop_max_h": 120, "icon": 7, "tint_role": 3,
                "scrollbar": 1, "scroll_axis": 2, "hc_force": 1,
                "border_sides": 5, "line_h": 24}
        rep = self._rep(node)
        self.assertFalse(rep.failed, rep.text_lines())

    def test_out_of_range_alpha_fails(self):
        node = {"id": 1, "kind": "rect", "layer": "work",
                "rect": {"x": 0, "y": 0, "w": 16, "h": 16}, "alpha": 300}
        rep = self._rep(node)
        self.assertTrue(any(r["rule"] == "C2" and r["status"] == "FAIL"
                             for r in rep.results))

    def test_bad_anim_ms_fails(self):
        node = {"id": 1, "kind": "rect", "layer": "work",
                "rect": {"x": 0, "y": 0, "w": 16, "h": 16}, "anim_ms": 120}
        rep = self._rep(node)
        self.assertTrue(any(r["rule"] == "C2" and r["status"] == "FAIL"
                             for r in rep.results))

    def test_bad_tint_role_fails(self):
        node = {"id": 1, "kind": "icon", "layer": "work",
                "rect": {"x": 0, "y": 0, "w": 16, "h": 16}, "tint_role": 9}
        rep = self._rep(node)
        self.assertTrue(any(r["rule"] == "C2" and r["status"] == "FAIL"
                             for r in rep.results))


class TestCap2AuditConsistency(unittest.TestCase):
    """A-9-05..10：真实规范 md ↔ schema ↔ 转换器一致性审计 RC=0。"""

    def test_real_md_audit_passes(self):
        md_path = os.path.join(TOOL_DIR, "..", "..", "docs",
                               "HTML-块树-转换规范.md")
        results, _ = validate.run_spec_audit(md_path)
        fails = [r for r in results if r["status"] == "FAIL"]
        self.assertEqual(fails, [], [f["detail"] for f in fails])
        rules = {r["rule"] for r in results}
        for rule in ("A-9-05", "A-9-06", "A-9-07", "A-9-08", "A-9-09",
                     "A-9-10"):
            self.assertIn(rule, rules)


class TestCap3Shape(unittest.TestCase):
    """cap3 增量：data-radius-shape / data-squircle-n（R-7.5-01..03）。"""

    def _node(self, attrs):
        el = html2blk.El("div", dict(attrs))
        node = {"kind": "column"}
        diag = html2blk.Diag()
        html2blk.apply_cap2(el, node, diag, "s")
        return node, diag

    def test_shape_arc_default(self):
        node, diag = self._node({"data-radius-shape": "arc"})
        self.assertFalse(diag.hard_fail, diag.lines())
        self.assertEqual(node["corner_shape"], 0)

    def test_shape_squircle_and_g2_alias(self):
        node, diag = self._node({"data-radius-shape": "squircle"})
        self.assertFalse(diag.hard_fail, diag.lines())
        self.assertEqual(node["corner_shape"], 1)
        node, diag = self._node({"data-radius-shape": "g2"})
        self.assertFalse(diag.hard_fail, diag.lines())
        self.assertEqual(node["corner_shape"], 1)

    def test_shape_bad_diagnosed(self):
        node, diag = self._node({"data-radius-shape": "blob"})
        self.assertTrue(diag.hard_fail)
        self.assertTrue(any(i["prop"] == "E_RADIUS_SHAPE_BAD"
                             for i in diag.items))

    def test_squircle_n_units(self):
        node, diag = self._node({"data-radius-shape": "squircle",
                                  "data-squircle-n": "4.5"})
        self.assertFalse(diag.hard_fail, diag.lines())
        self.assertEqual(node["corner_n"], 45)
        node, diag = self._node({"data-radius-shape": "squircle",
                                  "data-squircle-n": "5"})
        self.assertEqual(node["corner_n"], 50)

    def test_squircle_n_bad_exponent_hard_fail(self):
        node, diag = self._node({"data-radius-shape": "squircle",
                                  "data-squircle-n": "abc"})
        self.assertTrue(diag.hard_fail)
        self.assertTrue(any(i["prop"] == "E_RADIUS_N_BAD"
                             for i in diag.items))

    def test_squircle_n_out_of_range_clamped_warn(self):
        node, diag = self._node({"data-radius-shape": "squircle",
                                  "data-squircle-n": "8"})
        self.assertFalse(diag.hard_fail, diag.lines())
        self.assertEqual(node["corner_n"], 50)
        self.assertTrue(any(i["prop"] == "WARN_RADIUS_N_RANGE"
                             and i["level"] == "WARN" for i in diag.items))

    def test_corner_n_dropped_under_arc(self):
        node, diag = self._node({"data-radius-shape": "arc",
                                  "data-squircle-n": "4.5"})
        self.assertNotIn("corner_n", node)

    def test_c2_shape_n_ranges(self):
        good = {"id": 1, "kind": "rect", "layer": "work",
                "rect": {"x": 0, "y": 0, "w": 16, "h": 16},
                "corner_shape": 1, "corner_n": 45}
        tree = {"schema": "catos.blk/1.0", "root": {
            "id": 0, "kind": "root", "layer": "work",
            "rect": {"x": 0, "y": 0, "w": 160, "h": 80}, "children": [good]}}
        rep = validate.Report()
        validate.validate_tree(tree, rep)
        self.assertFalse(rep.failed, rep.text_lines())

        bad = dict(good, corner_n=99)
        tree["root"]["children"] = [bad]
        rep = validate.Report()
        validate.validate_tree(tree, rep)
        self.assertTrue(any(r["rule"] == "C2" and r["status"] == "FAIL"
                             for r in rep.results))


if __name__ == "__main__":
    unittest.main()
