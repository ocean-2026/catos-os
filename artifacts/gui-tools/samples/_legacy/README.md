# 已废弃（DEPRECATED）

`_patch_decl.py` 是本轮回炉前的事后回填脚本：在 `html2blk` 产出 `.blk.json`
之后机械补 `resize` / `bleed` 字段，并按锚定视口重算 startmenu 子块坐标。

本轮起，`resize` / `bleed` / 锚点 / 层 / 溢出 / 拖拽等声明层字段一律由 HTML 的
`data-blk-*` 属性单入口表达（见 `tools/blkhtml/schema.json ::
html_subset.attributes.data-blk`）；FREE 子树坐标由 `html2blk` 布局递归直接得出。
**本脚本已不在生成链路上**，仅留档对照旧版 `.blk.json`，请勿再执行。
