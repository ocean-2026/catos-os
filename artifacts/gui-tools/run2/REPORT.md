# 第二轮「所见即内核所画」端到端证实报告（真实样例 settings）

- 基线：`feat/blkhtml` @ 89584c4；本轮未 git commit。
- 第一轮（fixture）产物与 `run/REPORT.md` 原样保留，未改动；本轮产物全部落在 `artifacts/gui-tools/run2/`。
- 构建：`kernel_v25/sh build.sh iso`；QEMU：`qemu-system-i386 -L /home/user/qemu-local/root/usr/share/qemu -m 256 -display none -boot d -cpu qemu32 -net none`，ISO-only。
- 本轮不再使用第一轮的绕过脚本 `run/gen_blkgen.py`；装配器 `run2/assemble_blkgen.py` 只做结构胶水（抽取工具 emit-c 的 spec 块逐字保留、剔除根记录 id=0、生成非根 desc 数组、桥接提交/标脏/几何 dump），不含任何 kind/颜色映射。

## ① 输入与规模

| 样例 | 路径 | sha256 | 块数 |
|---|---|---|---|
| settings（主证据） | `tools/blkhtml/samples/settings.blk.json` | `edb3ab43467484f663d35bfacdd5b54c87104db6969c41bb6181e6a95290f3a6` | 18（根 id0 + 非根 1..17） |
| browser-toolbar（贴边专项） | `tools/blkhtml/samples/browser-toolbar.blk.json` | `8137303d838d2b486ac3c208b2f96863ae585ffde10b09fd29bc24eb9b6f39bf` | 8（根 + 7 非根） |

同目录其余样例：`fullbleed`、`notepad`、`startmenu`（未纳入本轮对照）。

## ② settings：两档截图 + 几何比对 + 像素 diff

### 截图与像素 diff（800×600，480,000 像素；容差 0）

| 档位 | QEMU 截图 | 对照预览 | 差异像素 | 差异率 | 一致率 |
|---|---|---|---|---|---|
| app | `run2/qemu-app.png` | `run2/preview-app.png` | 441,658 | 92.0121% | 7.9879% |
| calm | `run2/qemu-calm.png` | `run2/preview-calm.png` | 442,266 | 92.1388% | 7.8612% |

左右对比图：`run2/compare-settings-app.png`、`run2/compare-settings-calm.png`。

### 几何比对（串口 BLKGEN dump vs JSON rect，容差 0）

两档（app / calm）结果完全相同：**14/17 非根块一致；根 id0 `(0,0,800,600)` 一致 → 合计 15/18**。

不一致的 3 块全部位于标题栏 row（id2）：

| id | 节点 | JSON rect | 内核 rect | 偏差 |
|---|---|---|---|---|
| 2 | 标题栏 row（space-between / cross-center，pad 8） | 0,0,784,40 | 8,0,784,40 | x +8（交叉轴横向居中） |
| 3 | 文本 "Settings" | 8,16,64,16 | 16,8,64,16 | x +8，y −8 |
| 4 | 按钮 "Close" | 72,8,64,24 | 720,8,64,24 | 主轴两端对齐（Close 靠右缘） |

其余 14 个非根块（id1/5/6/7/8/9/10/11/12/13/14/15/16/17）与根 id0 全部 OK。明细：`run2/geom-settings.log`；串口日志：`run2/serial-run.log`；比对脚本：`run2/compare_geom.py`。

**归因（见第 ⑥ 节）**：内核 `layout.c` 实现了 `XALN_CENTER`（116/120 行）与 `ALN_SPACE_BETWEEN`（142/172 行），按 spec 排布；而 `preview.py` 与 html2blk 的静态 rect 模型不建模主轴/交叉轴对齐（preview.py 内 grep 不到任何 align/space-between 处理），子节点按起始方向堆叠。即这 3 处是「静态 rect 基准未表达对齐」，不是内核几何错误。

### 像素差异分解

最大差异桶（整屏底色角色映射，见第 ⑥.1 节）：

| 档位 | 主差异桶 | 像素数 |
|---|---|---|
| app | 内核 bg `1E1E26` vs 预览 panel `2A2A34` | 430,119 |
| calm | 内核 bg `1E1E26` vs 预览 panel `2A2A34` | 350,998 |
| calm | 内核 bg `1E1E26` vs 预览 border `555A66` | 78,399 |

其余为字形抗锯齿/截断、描边口径、光标混入帧等残差（与第一轮同类）。

## ③ browser-toolbar：贴边方角核验（app 档）

### 截图与像素 diff

| 项 | 路径 / 数值 |
|---|---|
| QEMU 截图 | `run2/qemu-toolbar-app.png` |
| 对照预览 | `run2/preview-toolbar-app.png` |
| 左右对比图 | `run2/compare-toolbar-app.png` |
| 像素 diff | 44,895 / 480,000 = 9.3531%（一致率 90.6469%） |

### 几何

7 个非根块的 **x / w 全部与 JSON 一致**（按钮 0/48/96/144，input 192，Menu 672）；**y 全部偏移 +280**（内核把整行按 `align_cross=center` 在 600 高画面内垂直居中：`(600−40)/2=280`；预览按顶对齐 y=0）。明细：`run2/geom-toolbar.log`、`run2/serial-toolbar.log`。

### 贴边 / 方角像素核验（内核 app 档，行带 y=280..319）

- **四角方角：通过。** 顶边 y=280 整条为边框色 `555A66`；角点像素 `(0,280)/(1,280)/(47,280)/(48,280)` 等均为边框色，未见圆角切角或抗锯齿缺口；app 档无圆角掩码参与。
- **相邻按钮共享边：未通过（2px，非单条共享 1px）。** 三处竖缝 x=47–48、95–96、143–144 各渲染 2px 边框（每个邻居各画 1px 全边框）；stop/input 缝 x=191–192 同样 2px。
  - 根因：工具 emit-c 只把 `stroke.w=1` 落成 `border_w=1`，**丢弃了 `stroke.sides` / `flush_sides`**；内核 `paint.c` 的 `body_border` 在 `border_w>0` 时画四边，按侧别选边的能力未进 `blk_spec`。因此按 spec 应只画单侧的贴边（如按钮 2 仅 R 边），当前两端各画全边框，缝宽翻倍。
- 注：工具栏根 row 的垂直居中属对齐建模差异（同第 ② 节归因），与贴边核验无关。

## ④ 新 emit-c 产物核验（工具 7 项修复是否落地）

对工具直接产出的 `run2/settings.c`（未经绕过脚本）核验：

1. **无字符串颜色字面量**：`grep -nE '\.(fg|bg|accent) = "' settings.c` → 无命中（RC=1）。第一轮的 `.bg = "panel"` 不可编译问题已消除。
2. **颜色走 role 枚举 + INHERIT_COLOR**：
   - `INHERIT_COLOR` 出现 18 次（每 spec 一次）。
   - role 枚举统计：`ROLE_BG`×1、`ROLE_BAR`×3、`ROLE_ACCENT`×3、`ROLE_NONE`×47；无 `ROLE_PANEL` 输出（见第 ⑥.1 节）。
   - 关键行示例（`settings.c`）：
     - 根列 `id1`（fill panel）：`.role_fg = ROLE_NONE, .role_bg = ROLE_BG, .role_accent = ROLE_NONE`
     - 标题栏 `id2`（fill bar）：`.role_bg = ROLE_BAR`
     - 强调文本 `id8/id13`（fg accent）：`.role_fg = ROLE_ACCENT`
3. **kind 与 flow 对齐**：toolbar 片段 `spec_1=BLK_ROW`，按钮 `BLK_BUTTON`，地址栏 `BLK_INPUT`；settings 片段容器 `BLK_ROW`/`BLK_COLUMN` 与 `layout.flow` 一致（第一轮 kind 滞后于 flow 的问题已消除）。

## ⑤ 构建与护栏

| 检查 | 结果 | 日志 |
|---|---|---|
| `sh build.sh iso`（settings 数据） | **RC=0**，ISO 12,087,296 字节 | `run2/build.log` |
| `tools/check_link_layout.py build/kernel.pe --strict` | **RC=0**；348 条 `[link][warn]`，均为既有 arena-window 静态审计提示（非失败），数量与第一轮相同 | `run2/guard_strict.log` |
| toolbar 数据构建 | RC=0，ISO 12,083,200 字节（专项截图后已回切 settings 数据重建） | `run2/build-toolbar.log` |

## ⑥ 失败 / 不一致项归因与待拍板点

### 已定位

1. **panel 角色映射口径（本轮最大像素差异来源）**
   - 工具 `blk2bin.py` B6 映射把 `panel` 归到 `ROLE_BG`（源码注释「panel/bg 归 ROLE_BG」）；内核 `paint.c` 中 `ROLE_BG→tok.bg=1E1E26`、`ROLE_PANEL→tok.panel=2A2A34`，是两个独立角色；预览按 panel `2A2A34` 渲染。
   - 后果：settings 根列 fill=panel 在内核被画成 bg 色，整屏底色与预览不符，贡献了 app/calm 绝大部分差异像素。
   - 待拍板：工具是否改发 `ROLE_PANEL`（推荐，保留内核两个角色）；或 schema/内核侧合并 panel 与 bg。**本轮未改工具，也未改内核角色解析。**

2. **对齐未进入静态 rect 基准**
   - 内核支持 `ALN_CENTER/END/SPACE_BETWEEN` 与 `XALN_CENTER/END/STRETCH`；预览器与 html2blk 静态 rect 不建模对齐，子节点起始堆叠。
   - 后果：settings 标题栏 3 块、toolbar 整行的几何对不上基准；内核行为符合 spec，预览/JSON 基准落后。后续若要几何逐块全等，需先在预览/rect 模型补齐对齐语义，或在比对时把对齐类偏差单列为「基准未建模」。

3. **描边侧别（stroke sides / flush_sides）未进 blk_spec**
   - emit-c 只携带 `border_w`，侧别掩码丢失；内核边框画四边。后果：贴边相邻按钮缝宽 2px（见第 ③ 节）。
   - 待拍板：是否在 `blk_spec` 增加边框侧别掩码并由工具填充；属内核模型 + 工具两侧协同改动。

### 未完全解释 / 遗留

4. **id3 文本垂直位置**：row 交叉轴居中按内容高 24、文本高 16 应得 y=12，内核实测 y=8；文字叶子在交叉轴上的居中口径未对齐。影响面小，本轮未深挖内核布局分支。

5. **CE 路线本轮未重跑（已知缺口，非回归）**：按任务要求不重跑 `blkgen.ce`。现状维持第一轮结论——`validate --ce` 仍会判 FAIL：`BUTTON(12)/INPUT(13)/TERM(14)` 被 `decl.c validate()` 以 `E_DECL_BADREC=−39` 拒收；槽 72 记录结构不携带颜色/布局/样式字段。这是 ABI 设计边界，不是本轮引入的回归。

## ⑦ 与第一轮（fixture）结论的差异

| 维度 | 第一轮（run/，fixture 7 非根） | 第二轮（run2/，真实样例 settings 18 块 + toolbar 8 块） |
|---|---|---|
| 几何一致 | 7/7 两档全等（容差 0） | settings 14/17 非根（15/18 含根）；toolbar 0/7（仅 y 偏移） |
| 像素一致率 | app 98.700% / calm 97.228% | app 7.9879% / calm 7.8612%；toolbar app 90.6469% |
| 像素差异主因 | 光标、字形 AA、app 默认描边、calm 圆角/阴影残差 | 新增 **panel→ROLE_BG 整屏底色映射**（占绝对大头）；对齐建模差异；其余残差同类 |
| 工具侧缺陷 | 角色名写成字符串字面量（不可编译）；kind 滞后 flow | **两项均已修复并核验通过**：无字符串字面量、kind 跟 flow 对齐 |
| 绕过手段 | 只读辅助脚本 `gen_blkgen.py` 现场补角色/kind | 不再使用；装配器 `assemble_blkgen.py` 仅做结构胶水 |
| 未覆盖路线 | B CE 未取到可视帧；raw rc=−39 / proj rc=0 | 本轮不重跑 CE；−39 缺口如实保留为已知边界 |
| 护栏 | STRICT_RC=0，348 warn | STRICT_RC=0，348 warn（持平） |

## 附：本轮产物清单（`artifacts/gui-tools/run2/`）

- 报告：`REPORT.md`
- settings：`qemu-app.png`、`qemu-calm.png`、`preview-app.png`、`preview-calm.png`、`compare-settings-app.png`、`compare-settings-calm.png`
- toolbar：`qemu-toolbar-app.png`、`qemu-toolbar-calm.png`、`preview-toolbar-app.png`、`compare-toolbar-app.png`
- 数据/片段：`settings.c`、`settings.recs.bin`、`settings.strings.bin`、`toolbar.c`、`toolbar.recs.bin`、`toolbar.strings.bin`
- 日志：`serial-run.log`、`serial-toolbar.log`、`geom-settings.log`、`geom-toolbar.log`、`build.log`、`build-toolbar.log`、`guard_strict.log`
- 脚本：`drive_blkgen.py`、`compare_geom.py`、`diff_png.py`、`assemble_blkgen.py`、`make_composite.py`

## 附：改动文件口径

- 允许范围内：`kernel_v25/src/gui/blkgen.c`（数据驱动，工具 emit-c 片段 + 装配桥接，文件头保留 AUTO-GENERATED 与 source/sha）；`chrome.c`/`gui_int.h`/`build.sh`/`apps.list`/`extapp/blkgen.c` 沿用第一轮接线，本轮未再改动。
- 未触碰：`tools/blkhtml/*.py`、`schema.json`、`FROZEN.md`；`gui-hosted`/`gui-fix`/`gui-preview`；未提交 commit。
