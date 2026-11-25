#!/usr/bin/env python3
"""Auxiliary read-only generator (NOT part of tools/blkhtml; contract frozen).

Reads a .blk.json and emits kernel_v25/src/gui/blkgen.c:
  - static blk_spec / blk_desc arrays (full-fidelity built-in page route)
  - blkgen_build(): set root 800x600, gui_commit_desc(), layout_run(),
    dump per-block geometry to the serial port as BLKGEN id=.. x=.. ...

Role-color names are mapped to ROLE_* enum members (the blk2bin --emit-c
fragment currently writes the role name as a quoted string into the uint32
fg/bg/accent fields; that tool-side defect is recorded in the report and
worked around here without touching the frozen tools).

Root record id=0 is NOT submitted: parent=0 attaches to gui_root_block().
"""
import json
import sys

KIND_ENUM = {
    "root": "BLK_ROOT", "column": "BLK_COLUMN", "row": "BLK_ROW",
    "stack": "BLK_STACK", "text": "BLK_TEXT", "rect": "BLK_RECT",
    "image": "BLK_IMAGE", "surface": "BLK_SURFACE", "spacer": "BLK_SPACER",
    "divider": "BLK_DIVIDER", "canvas": "BLK_CANVAS", "free": "BLK_FREE",
    "button": "BLK_BUTTON", "input": "BLK_INPUT", "term": "BLK_TERM",
}
ALN_MAIN = {"start": "ALN_START", "center": "ALN_CENTER", "end": "ALN_END",
            "space_between": "ALN_SPACE_BETWEEN"}
ALN_CROSS = {"start": "XALN_START", "center": "XALN_CENTER",
             "end": "XALN_END", "stretch": "XALN_STRETCH"}
OVF = {"clip": "OVF_CLIP", "visible": "OVF_VISIBLE", "scroll": "OVF_SCROLL"}
DRAG = {"none": "DRAG_NONE", "title": "DRAG_TITLE", "full": "DRAG_FULL"}
BOUND = {"free": "BOUND_FREE", "parent": "BOUND_PARENT", "screen": "BOUND_SCREEN"}
ANCHOR = {"L": "ANC_L", "R": "ANC_R", "T": "ANC_T", "B": "ANC_B",
          "HCENTER": "ANC_HCENTER", "VCENTER": "ANC_VCENTER"}
ROLE = {"fg": "ROLE_FG", "bg": "ROLE_BG", "panel": "ROLE_PANEL",
        "bar": "ROLE_BAR", "border": "ROLE_BORDER", "line": "ROLE_LINE",
        "accent": "ROLE_ACCENT"}
LAYER = {"bg": 0, "content": 100, "work": 200, "chrome": 3000,
         "overlay": 4000, "toast": 5000}


def c_str(s):
    return json.dumps(s, ensure_ascii=False)


def color_expr(val):
    """Return (fg/bg/accent literal, role literal) for a color declaration."""
    if not val:
        return "INHERIT_COLOR", "0"
    if val.startswith("#"):
        return "0x" + val[1:].upper(), "0"
    if val in ROLE:
        return "INHERIT_COLOR", ROLE[val]
    raise ValueError("unhandled color %r" % val)


def collect(node, acc):
    acc.append(node)
    for ch in node.get("children", []):
        collect(ch, acc)


def pad4(pad, default=0):
    if isinstance(pad, int):
        return (pad, pad, pad, pad)
    return (pad.get("t", default), pad.get("r", default),
            pad.get("b", default), pad.get("l", default))


def kind_for(n):
    """Kernel kind for a node.  Containers carry both a `kind` label and a
    `layout.flow` directive; the kernel axis follows flow (the fixture's
    kind label can lag the flow), so flow wins for column/row/stack.
    """
    k = n["kind"]
    if k in ("column", "row", "stack"):
        flow = (n.get("layout") or {}).get("flow")
        if flow in ("column", "row", "stack"):
            return flow
    return k


def spec_init(n):
    req = n.get("req", {}) or {}
    pad = pad4(n.get("padding", 0))
    mg = pad4(n.get("margin", 0))
    layout = n.get("layout", {}) or {}
    pct = n.get("pct", {}) or {}
    px = n.get("px", {}) or {}
    mm = n.get("min_max", {}) or {}
    resize = n.get("resize", {}) or {}
    scroll = n.get("scroll", {}) or {}
    font = n.get("font", {}) or {}
    stroke = n.get("stroke", {}) or {}

    fg_lit, fg_role = color_expr(n.get("fg"))
    bg_lit, bg_role = color_expr(n.get("fill"))
    ac_lit, ac_role = color_expr(n.get("accent"))

    anchors = " | ".join(ANCHOR[a] for a in n.get("anchor", [])) or "0"
    resize_dir = " | ".join("RESIZE_" + d
                            for d in resize.get("dir", [])) or "0"

    lines = []
    lines.append("    .req_w = %d, .req_h = %d," % (req.get("w", -1),
                                                    req.get("h", -1)))
    lines.append("    .pad_t = %d, .pad_r = %d, .pad_b = %d, .pad_l = %d,"
                 % pad)
    lines.append("    .gap = %d," % layout.get("gap", 0))
    lines.append("    .align_main = %s, .align_cross = %s,"
                 % (ALN_MAIN.get(layout.get("align_main", "start"), "ALN_START"),
                    ALN_CROSS.get(layout.get("align_cross", "start"),
                                   "XALN_START")))
    lines.append("    .layer = %d, .z = %d,"
                 % (LAYER.get(n.get("layer", "work"), 200), n.get("z", 0)))
    lines.append("    .visible = %d, .focusable = %d, .inert = %d,"
                 % (1 if n.get("visible", True) else 0,
                    1 if n.get("focusable", False) else 0,
                    1 if n.get("inert", False) else 0))
    lines.append("    .fg = %s, .bg = %s, .accent = %s,"
                 % (fg_lit, bg_lit, ac_lit))
    lines.append("    .role_fg = %s, .role_bg = %s, .role_accent = %s,"
                 % (fg_role, bg_role, ac_role))
    lines.append("    .scale = %d, .border_w = %d,"
                 % (font.get("scale", 1), stroke.get("w", 0)))
    lines.append("    .overflow = %s,"
                 % OVF.get(n.get("overflow", "clip"), "OVF_CLIP"))
    lines.append("    .anchor = %s," % anchors)
    lines.append("    .pct_x = %d, .pct_y = %d, .pct_w = %d, .pct_h = %d,"
                 % (pct.get("x", 0), pct.get("y", 0),
                    pct.get("w", 0), pct.get("h", 0)))
    lines.append("    .px_x = %d, .px_y = %d," % (px.get("x", 0), px.get("y", 0)))
    lines.append("    .min_w = %d, .min_h = %d, .max_w = %d, .max_h = %d,"
                 % (mm.get("min_w", 0), mm.get("min_h", 0),
                    mm.get("max_w", 0), mm.get("max_h", 0)))
    lines.append("    .m_t = %d, .m_r = %d, .m_b = %d, .m_l = %d," % mg)
    lines.append("    .drag_mode = %s, .drag_bounds = %s,"
                 % (DRAG.get(n.get("drag", "none"), "DRAG_NONE"),
                    BOUND.get(n.get("drag_bounds", "free"), "BOUND_FREE")))
    lines.append("    .resize_dir = %s, .resize_edge = %d,"
                 % (resize_dir, resize.get("edge", 6)))
    lines.append("    .resize_min_w = %d, .resize_min_h = %d,"
                 % (resize.get("min_w", 24), resize.get("min_h", 24)))
    lines.append("    .resize_max_w = %d, .resize_max_h = %d, .resize_step = %d,"
                 % (resize.get("max_w", 0), resize.get("max_h", 0),
                    resize.get("step", 0)))
    lines.append("    .scroll_x = %d, .scroll_y = %d,"
                 % (scroll.get("x", 0), scroll.get("y", 0)))
    return "\n".join(lines)


def main():
    src, out = sys.argv[1], sys.argv[2]
    tree = json.load(open(src, encoding="utf-8"))
    nodes = []
    collect(tree["root"], nodes)
    by_id = {n["id"]: n for n in nodes}

    out_lines = []
    out_lines.append("/* AUTO-GENERATED by artifacts/gui-tools/run/gen_blkgen.py")
    out_lines.append(" * from %s. Do not edit by hand." % src)
    out_lines.append(" *")
    out_lines.append(" * Full-fidelity built-in page: blk_spec all fields,")
    out_lines.append(" * committed through gui_commit_desc (NOT CE slot 72).")
    out_lines.append(" * Root record id=0 is excluded: parent=0 attaches to")
    out_lines.append(" * gui_root_block().")
    out_lines.append(" */")
    out_lines.append("#include \"gui/gui.h\"")
    out_lines.append("#include \"gui_int.h\"")
    out_lines.append("#include \"serial_log.h\"")
    out_lines.append("")
    out_lines.append("static void blkgen_put_dec(int32_t v) {")
    out_lines.append("    char b[12]; int n = 0;")
    out_lines.append("    if (v < 0) { ser_putc('-'); v = -v; }")
    out_lines.append("    if (v == 0) { ser_putc('0'); return; }")
    out_lines.append("    while (v) { b[n++] = (char)('0' + v % 10); v /= 10; }")
    out_lines.append("    while (n) ser_putc(b[--n]);")
    out_lines.append("}")
    out_lines.append("")

    for n in nodes:
        i = n["id"]
        out_lines.append("static const blk_spec SPEC_%d = {" % i)
        out_lines.append(spec_init(n))
        out_lines.append("};")
        out_lines.append("")

    descs = [n for n in nodes if n["id"] != 0]
    for n in descs:
        i = n["id"]
        parent = n.get("ce", {}).get("parent", 0)
        text = c_str(n["text"]) if n.get("text") else "0"
        pix = n.get("pix", {}) or {}
        out_lines.append(
            "static const blk_desc DESC_%d = { .id = %d, .kind = %s,"
            " .parent = %d, .spec = &SPEC_%d, .text = %s,"
            " .pix = 0, .pix_w = %d, .pix_h = %d };"
            % (i, i, KIND_ENUM[kind_for(n)], parent, i, text,
               pix.get("w", 0), pix.get("h", 0)))
    out_lines.append("")

    out_lines.append("int blkgen_build(void) {")
    out_lines.append("    block *root = gui_root_block();")
    out_lines.append("    root->w = 800; root->h = 600;")
    out_lines.append("    gui_commit_desc(blkgen_descs,"
                     " sizeof(blkgen_descs) / sizeof(blkgen_descs[0]));")
    out_lines.append("    /* 提交的子树默认不带脏标记，首帧快照会跳过它们；")
    out_lines.append("     * 逐块标脏，保证下一帧整组重绘。 */")
    out_lines.append("    for (uint32_t i = 0; i < sizeof(blkgen_descs) /")
    out_lines.append("                     sizeof(blkgen_descs[0]); i++)")
    out_lines.append("        gui_mark_dirty(blkgen_descs[i].id);")
    out_lines.append("    gui_mark_dirty(root->id);")
    out_lines.append("    layout_run();")
    out_lines.append("")
    out_lines.append("    uint32_t cap = btable_capacity();")
    out_lines.append("    for (uint32_t id = 1; id <= cap; id++) {")
    out_lines.append("        block *b = btable_get(id);")
    out_lines.append("        if (!b) continue;")
    out_lines.append("        ser_write(\"BLKGEN id=\"); blkgen_put_dec(b->id);")
    out_lines.append("        ser_write(\" x=\"); blkgen_put_dec(b->x);")
    out_lines.append("        ser_write(\" y=\"); blkgen_put_dec(b->y);")
    out_lines.append("        ser_write(\" w=\"); blkgen_put_dec(b->w);")
    out_lines.append("        ser_write(\" h=\"); blkgen_put_dec(b->h);")
    out_lines.append("        ser_write(\"\\n\");")
    out_lines.append("    }")
    out_lines.append("    return 0;")
    out_lines.append("}")
    out_lines.append("")

    # desc array reference must sit after the DESC_ definitions.
    body = "\n".join(out_lines)
    body = body.replace(
        "int blkgen_build(void) {",
        "static const blk_desc blkgen_descs[] = {\n"
        + ",\n".join("    DESC_%d" % n["id"] for n in descs)
        + ",\n};\n\n"
        "int blkgen_build(void) {")

    open(out, "w", encoding="utf-8").write(body + "\n")
    print("wrote %s (%d non-root descs)" % (out, len(descs)))


if __name__ == "__main__":
    main()
