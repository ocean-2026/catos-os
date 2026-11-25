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


if __name__ == "__main__":
    unittest.main()
