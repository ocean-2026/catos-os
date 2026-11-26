# blkhtml 五样例证据报告（第三轮 · 冻结前最后一轮工具修复）

- 工作树：`/home/user/work/gui-tools`；工具目录 `tools/blkhtml/`
- 生成日期：2026-11-25（Asia/Shanghai）
- 契约：`tools/blkhtml/schema.json`（catos.blk/1.0）；内核事实与预览口径：`FROZEN.md`
- 屏幕档：800×600；主题 dark；空间档仅用 {0,4,8,12,16,24}；颜色只用令牌角色名。
- 红线遵守：未改内核源码；未 `git commit`；未进入 `gui-hosted` / `gui-fix`；未写 `gui-preview`。
- 测试：`python3 tests/run_tests.py` → **Ran 29 tests, OK**（含 1 条 emit-c `gcc` 编译冒烟）。

## 1. 总览

| 样例 | 块数 | 文本字节 | 池字节 | recs 字节 | 裸树 validate | CE 校验 | 预览 |
|---|---:|---:|---:|---:|---|---|---|
| settings | 18 | 79 | 86 | 720 | `FAIL=0`，退出 0 | `FAIL=1`（root 投影） | app, calm |
| startmenu | 20 | 118 | 87 | 800 | `FAIL=0`，退出 0 | `FAIL=17`（free/overlay 非 CE 可还原） | app, calm |
| notepad | 9 | 47 | 51 | 360 | `FAIL=0`，退出 0 | `FAIL=2`（button/input 槽 72 拒收） | app |
| browser-toolbar | 8 | 26 | 32 | 320 | `FAIL=0`，退出 0 | `FAIL=6`（button/input 槽 72 拒收） | app, design |
| fullbleed | 6 | 48 | 51 | 240 | `FAIL=0`，退出 0 | `FAIL=0` | app |

- 裸树（`validate.py`，非 `--ce`）：五样例全部 `FAIL=0`、退出 0。
- CE 投影（`validate.py --ce`）：`free` 定位、`button/input` 几何与角色、free 子树坐标在槽 72 不可还原，按预期 FAIL；这是协议缺口，不是工具回归。
- 二进制 P1 复核：五样例 `--bin --recs --strings` 全部 `FAIL=0`，记录步长 40B/条，`pad0=0`，`pix_off=-1`。

## 2. 已冻结口径（本轮未变）

1. **CE 记录步长 40 字节/条**：`REC_BYTES = 40`，`REC_FMT = "<i i B B H i i i i i i i"`；`pix_off` 恒 `-1`；`pad0` 恒 0；CE 池只装去重文本。
2. **配额**：max_blocks 256、max_depth 16、max_text_bytes 4096、max_pool_bytes 32768。
3. **kind 由 flex-direction 决定**：`flex-direction:row → row`，`column → column`；`stack`/`free` 例外保留；emit-c 与 JSON 同一口径，不信 JSON 里旧的 kind 字段。
4. **颜色角色枚举（emit-c）**：角色名直给 `ROLE_FG/ROLE_BG/ROLE_PANEL/ROLE_BAR/ROLE_ACCENT/ROLE_BORDER/ROLE_LINE`，对应 `.bg/.fg/.accent = INHERIT_COLOR`；`#RRGGBB` 才烧进颜色字面量且 role 置 `ROLE_NONE`。
5. **CE kind 白名单**：`--ce` 口径下 kind 必须 ∈ `{column,row,stack,text,rect,image,surface,spacer,divider,canvas}`；`button/input/term` 一律拒收（`E_DECL_BADREC=-39`）。
6. **ABI 冻结**：不改 `blk_spec` 布局；不改内核源码；emit-c 只保证编到 `.o`，内置页接线由后续子代理负责。

## 3. 工具修复台账

### 3.1 前两轮已修（回归保持）

| 编号 | 问题 | 修法 | 单测 |
|---|---|---|---|
| B1 | `free` 子树递归落位丢失 | `place_free` 对 anchor/px/pct 全量递归，子块坐标相对父内容框 | `test_free_subtree_coordinates` |
| B2 | `gap:4px` 被拒 | 正则接受 `N` 与 `Npx`；非 8 档仍 FAIL | `test_gap_4px_parsed` / `test_gap_bad_ladder_still_unsupported` |
| B3 | 描边角色名解析失败 | `resolve()` 统一解析角色名/hex；渲染无角色不再抛错 | `test_role_name_resolved` / `test_hex_passthrough` / `test_render_no_role_value_error` |
| B4 | 滚动/裁剪越界只 WARN | 越界滚动 + `overflow:visible` 记 WARN；裁剪越界仍 FAIL | `test_scroll_outside_content_warns_not_fails` / `test_clip_outside_content_fails` |
| B5 | `<input>` 未自动闭合导致兄弟错挂 | void 元素进栈后立即闭合 | `test_input_then_sibling_parent` |
| B6 | emit-c 把角色名写成 C 字符串字面量，编不过 | 角色名→枚举 + `INHERIT_COLOR`；hex→`0xRRGGBBu` | `test_role_maps_to_role_enum` / `test_hex_burned_into_color` / `test_emit_c_uses_role_not_string_literal` |
| B7 | 容器 kind 与 `flex-direction` 不一致 | kind 由 flow 映射；emit-c 同口径 | `test_row_div_kind_is_row` / `test_column_div_kind_is_column` |
| CE | `--ce` 未按内核 `validate()` 拒收 button/input/term | L1 加 kind 白名单；非 `--ce` 口径不受影响 | `test_button_rejected_under_ce` / `test_plain_mode_accepts_button` |

### 3.2 本轮（第三轮）修复

| 编号 | 问题 | 修法 | 代码位置 | 单测 |
|---|---|---|---|---|
| P1 | `fill:"panel"` 被错映射到 `ROLE_BG`；内核 `ROLE_BG=1E1E26`、`ROLE_PANEL=2A2A34` 是两个角色，整屏底色错成桌面 bg | 角色名直给：`panel→ROLE_PANEL`、`bar→ROLE_BAR`、`bg→ROLE_BG`、`border→ROLE_BORDER`、`line→ROLE_LINE`、`accent→ROLE_ACCENT`、`fg→ROLE_FG`；不再用 layer 反推。preview 侧 `resolve()` 本就走 `token_palette[panel]`，核对无误，未改 | `blk2bin.py :: ROLE_ENUM`、`color_expr()` | `test_panel_role_emitted`（断言 `.role_bg = ROLE_PANEL` 且 `.bg = INHERIT_COLOR`）、`test_role_maps_to_role_enum` |
| P2 | html2blk 矩形求解未建模主轴/交叉轴对齐；space-between、align-items:center 与内核 `layout_run()` 不一致 | 照抄 `kernel_v25/src/gui/layout.c` 的 `layout_row`/`layout_column`：主轴 `ALN_START/CENTER/END/SPACE_BETWEEN`（space_between `slot=extra/(n-1)`），交叉轴 `XALN_START/CENTER/END/STRETCH`，padding/gap 参与、`align_self` 覆盖；落位坐标照内核公式原值写入，不归整 8（尺寸仍归整） | `html2blk.py :: place_children()`（row/column 分支）、`place_free()` | `test_row_space_between`、`test_row_align_items_center` |

## 4. 样例证据

### 4.1 settings（18 块 · 池 86 · recs 720）

- 树：root → column；标题栏 row（space-between）→ 文本「Settings」+ Close 按钮；列表行 row ×2；底部提示行。
- 标题栏反例（row + space-between，父 784×40、pad_l=8）：
  - 文本 id3：`{x:8, y:8, w:64, h:16}`
  - 按钮 id4：`{x:712, y:8, w:64, h:24}`，右缘贴内容框右 776。
- 产物：
  - `settings.blk.json` sha256 `0acdc677f80d775320b6b798e81e81cb81eb49d42cea83efa44bd2df585cc64a`，mtime 2026-11-25 19:53:38
  - `settings-app.png` sha256 `c1f1fb72e79c791df9d70598c3ecb20ba6cddba27527b973a6266bf1c184c4a6`
  - `settings-calm.png` sha256 `197bb1d1055bff38e1892f79bdae481973a25b3e998c764e2fa74508d867099d`
- 校验：裸树 `FAIL=0`；CE `FAIL=1`（root id=0 投影丢弃）；P1 `FAIL=0`。

### 4.2 startmenu（20 块 · 池 87 · recs 800）

- 树：root → free overlay；用户行 anchor 底；分隔线显式 `width:240px;height:8px;top:368px`；菜单项 column 堆叠。
- 产物：
  - `startmenu.blk.json` sha256 `7886a4f1a1f867184379493b533f1f4fb1dadca4d9cbaea7669d591508049a44`，mtime 2026-11-25 19:53:39
  - `startmenu-app.png` sha256 `a663bd61fa076afc635fce6f81945578ca942f1cfdc957055f7beea0d97ea7bd`
  - `startmenu-calm.png` sha256 `8f6000474d3b16db3cc93f9fc8e0161ed74e9dc55fb1e74bc358bd37f21c0f5f`
- 校验：裸树 `FAIL=0`；CE `FAIL=17`（free 定位/anchor/overlay 非槽 72 可还原）；P1 `FAIL=0`。

### 4.3 notepad（9 块 · 池 51 · recs 360）

- 树：root → column；标题栏 row（space-between）→ 文本 + Close；正文文本；底部 row → input。
- 产物：
  - `notepad.blk.json` sha256 `3bd40cb4614e6f71f570f6305dd930f9bb96bb7da8ab0237c102d9641058f8d2`，mtime 2026-11-25 19:53:39
  - `notepad-app.png` sha256 `2d36207ec002e01c51f5a5614a2a608dc4beb5ae423ad1f0da643fd1e1ad0d9e`
- 校验：裸树 `FAIL=0`；CE `FAIL=2`（button/input 槽 72 拒收）；P1 `FAIL=0`。

### 4.4 browser-toolbar（8 块 · 池 32 · recs 320）

- 树：root → row；4 个导航按钮 + 居中 input（h24，父 h40 → y=8）+ 菜单按钮。
- 交叉轴反例（row + align-items:center，父高 48、子高 24）：子 `y=(48-24)/2=12`（单测覆盖；样例父高 40、子高 24 → y=8，落 8 网格）。
- 产物：
  - `browser-toolbar.blk.json` sha256 `8137303d838d2b486ac3c208b2f96863ae585ffde10b09fd29bc24eb9b6f39bf`，mtime 2026-11-25 19:53:40
  - `browser-toolbar-app.png` sha256 `9bb76ed38199dd27926b770b5ea15b5b088bcac340cf1b839232af31080fa77a`
  - `browser-toolbar-design.png` sha256 `2098075fefdd26f024e6cbe9a749a59b6e5c7319d493e0c093fcc7444603059a`
- 校验：裸树 `FAIL=0`；CE `FAIL=6`（button/input 槽 72 拒收）；P1 `FAIL=0`。

### 4.5 fullbleed（6 块 · 池 51 · recs 240）

- 树：root → column；横幅 `data-blk-bleed="true"`；内容卡片。
- 产物：
  - `fullbleed.blk.json` sha256 `263add68c522893ecf183da1b90953bf748762772cf476803286b8822e02cff3`，mtime 2026-11-25 19:53:40
  - `fullbleed-app.png` sha256 `6ebc391da14686b2f0dd4793bdd966849800933f232f67d7c8a6da598acd87fd`
- 校验：裸树 `FAIL=0`；CE `FAIL=0`；P1 `FAIL=0`。

## 5. 残留问题扫描

- 五个样例 `UNSUPPORTED` 计数均为 0；无 `--allow-lossy` 降级。
- 未发现：`pix_off != -1`、`pad0 != 0`、记录步长非 40B、kind 与 flow 不一致、角色名被写成 C 字符串字面量、越界裁剪、free 子树坐标丢失。
- 编译冒烟：v2 `sample.c` 经 `gcc -m32 -ffreestanding -Wall -Wextra -I<kernel_v25>/include -c` 编到 `/tmp/sample.o`，退出 0、无 warning。

## 6. v2 fixture 链路（tests/fixtures/sample.html）

- `v2/sample.blk.json` sha256 `53ebb832a0576cc32a2b5d30be68bec35691731c015ab30575bc200cbd040988`，mtime 2026-11-25 19:54:08（fixture 树未变，哈希与上轮一致）。
- 8 块 / 文本 50 / 池 54 / recs 320（40×8）。
- `sample-app.png` `38def4816d3571b594d4c71b23f9766ba57fd57919cd515cd08885b5f7af8bbf`
- `sample-calm.png` `9659335376adf6ce1ee7551920b04a01e1caefcaf0317c7443109626e22acb53`
- `sample-design.png` `c7bf75e888d98933449e10cda8e924594c9dd5f607b4deef9ef559833aa65f8b`
- emit-c 角色输出核对：面板块 `.role_bg = ROLE_PANEL`，工具条 `.role_bg = ROLE_BAR`，纯色块 `.role_bg = ROLE_NONE`。

## 7. 已知偏差 / ABI 缺口

1. **描边侧别（ABI 缺口，不改内核）**：`blk_spec` 只有 `border_w`、没有侧别掩码；内核 `body_border` 对 `border_w>0` 一律画四边。校验器与 design 预览里的「贴边侧省略描边」只在 design 预览成立；内核内置页会画四边（2px 共享缝）。本轮不为它改内核。
2. **交叉轴居中与 8pt 网格**：内核布局不归整子块坐标；工具 schema G1 要求 x/y/w/h 为 8 的倍数。居中/端对齐可能算出非网格坐标（如 48 高父框内 24 高子块 y=12），G1 会判 FAIL；样例 HTML 选能落到网格上的档位，不在工具内放宽断言。
3. **文本叶子交叉轴**：内核实测 settings id3 文本 y=8（父内容 y=8），即文本叶子默认按 `XALN_START` 落内容框顶、高度 16，不做交叉轴居中；工具按此口径实现。其余文本叶子在 center/end 档下与内核是否逐块相等，未在本轮真机逐块复核。
4. **CE 通道不携带几何/样式**：槽 72 提交后 rect、padding/gap/align、角色色、描边等全部丢失；全保真只走 `--emit-c` 内置页片段。
5. **CE 树桌面显示路径不存在**：`gui_begin()→gui_reset()→tree_reset()` 会清空根下所有子树（`compose.c:326-348`、`tree.c:67-75`）；本轮未动内核，也不据此调整工具。
