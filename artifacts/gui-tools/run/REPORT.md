# 「所见即内核所画」端到端证实报告

- 基线：`feat/blkhtml` @ 89584c4
- 证据 fixture：`artifacts/gui-tools/sample.blk.json`（1 根 + 7 非根块）
- 构建：`kernel_v25/sh build.sh iso`（RC=0）；ISO：`kernel_v25/catos.iso`（12,083,200 字节）
- QEMU：`/home/user/.local/bin/qemu-system-i386 -L /home/user/qemu-local/root/usr/share/qemu -m 256 -display none -boot d -cpu qemu32 -net none`，ISO-only（无 -hda）；CE 阶段另挂 `/tmp/blktest.img`（overlay，避开 /home/user hpvs_fs 的 flock 拒绝）。

## ① 截图与像素 diff

| 路线/档位 | QEMU 截图 | 对照预览 | 差异像素 | 占比 | 一致率 |
|---|---|---|---|---|---|
| A 内置页 app | `run/qemu-app.png` | `run/preview-app.png` | 6,238 / 480,000 | 1.300% | 98.700% |
| A 内置页 calm | `run/qemu-calm.png` | `run/preview-calm.png` | 13,304 / 480,000 | 2.772% | 97.228% |
| B CE 应用 | `run/qemu-ce.png` | （无像素对照，见缺口清单） | — | — | — |

app 档差异区域：top strip(y<60) 3,998 px，其余 2,240 px；字形类 1,874 px，底色/描边类 4,364 px。
calm 档差异区域：top strip 10,724 px，其余 2,580 px；字形类 1,868 px，底色/圆角/阴影类 11,436 px。

## ② 几何比对（串口 BLKGEN dump vs JSON rect，容差 0）

两档（app / calm）各 7/7 块坐标、尺寸全部一致：

| id | JSON (x,y,w,h) | 内核 (x,y,w,h) | 结果 |
|---|---|---|---|
| 1 | 0,0,800,600 | 0,0,800,600 | OK |
| 2 | 8,8,360,24 | 8,8,360,24 | OK |
| 3 | 8,8,120,24 | 8,8,120,24 | OK |
| 4 | 128,8,120,24 | 128,8,120,24 | OK |
| 5 | 248,8,120,24 | 248,8,120,24 | OK |
| 6 | 8,32,328,32 | 8,32,328,32 | OK |
| 7 | 16,40,312,16 | 16,40,312,16 | OK |

汇总：app 7/7，calm 7/7，无差异行。串口日志：`run/serial-run.log`；比对脚本：`run/compare_geom.py`。

## ③ 三条路线结论表（同一份 sample.blk.json）

| 维度 | A 内核内置页（blk_spec 全保真） | B CE 应用（槽 72 结构保真） | C 离线预览（preview.py） |
|---|---|---|---|
| 块数 | 7 非根 | raw 7 / proj 7 | 7 非根 |
| 坐标一致块数 | 7/7（容差 0） | 不提交几何；proj 提交后由内核按自然尺寸排版 | 基准（JSON rect） |
| 像素一致率 | app 98.700% / calm 97.228% | 未取到可视帧（见归因） | — |
| 颜色/角色 | 全量（fg/bg/accent + role_*） | 全丢（记录无颜色字段） | 全量 |
| 圆角/描边/阴影 | 由 blk_spec 携带，内核渲染 | 全丢 | 全量 |
| 布局约束（padding/gap/align/锚点/百分比/边距/拖拽/缩放/滚动） | 全量 | 全丢 | 全量 |
| 可提交种类 | 全部 kind | 仅 COLUMN..CANVAS；FREE/BUTTON/INPUT/TERM 拒收 | — |
| 提交 verdict | gui_commit_desc 成功 | raw rc=-39 (E_DECL_BADREC)；proj rc=0，n=7，leaked=0 | — |
| 复现命令 | `gui shot-style-app blkgen`；`gui shot-style-calm blkgen` | shell 内 `blkgen` | `preview.py sample.blk.json --png out.png --mode app|calm --theme dark` |

### B 路线缺口清单（槽 72 记录 vs blk_spec）

**记录携带**：id, parent, kind, layer, req_w, req_h, text_off, pix_off, pix_w, pix_h, res。

**经 CE 通道丢失**：
- 颜色：fg / bg / accent 字面量，role_fg / role_bg / role_accent
- 盒模型：pad_t/r/b/l、gap、align_main、align_cross
- 排版：font scale、border_w、overflow
- 定位：anchor、pct_x/y/w/h、px_x/y
- 约束：min/max w/h、m_t/r/b/l
- 交互：drag_mode、drag_bounds、resize_dir/edge/min/max/step
- 滚动：scroll_x/y
- 状态：z、visible、focusable、inert
- 外观：stroke sides、radius、shadow（schema 层字段，本就不进 blk_spec）

**种类/层限制**：kind 仅接受 BLK_COLUMN(1)..BLK_CANVAS(10)；FREE(11)/BUTTON(12)/INPUT(13)/TERM(14) 拒收；layer 仅 WORK。

## ④ 护栏

- `check_link_layout.py build/kernel.pe --strict`：**STRICT_RC=0**；日志 `run/guard_strict.log`，348 条 `[link][warn]` 均为既有 arena-window 静态审计提示（非失败）。
- `sh build.sh iso`：RC=0。

## ⑤ 失败/不一致项归因

1. **像素差异（app 1.30% / calm 2.77%）**
   - 鼠标光标：QEMU 帧含箭头光标（预览无），落在画面中部，计入背景类差异。
   - 字形抗锯齿：内核 8x16 位图字体与预览字体在边缘像素上不完全重合；内核文本整串绘制，预览按 ellipsis 截断（id7 末尾）。
   - app 档默认描边：预览对非文本叶子补 1px 描边；内核仅在 `border_w>0` 时画边，fixture 未显式给 border_w → 按钮边缘出现底色/描边类差异。**归因：工具/内核默认值口径差异，非几何错误。**
   - calm 档圆角/阴影：内核 calm 渲染与预览 calm 掩码在圆角半径、双层阴影上存在像素级出入。**归因：两端 calm 外观算法未逐像素对齐，属渲染细节，不影响几何。**
2. **B 路线 raw 批次 rc=-39**：fixture 含 3 个 BUTTON(kind=12)，超出槽 72 可提交范围（>BLK_CANVAS=10），被 `decl.c` validate 以 E_DECL_BADREC 拒收。**归因：ABI 种类覆盖限制（设计如此），不是工具 bug。**
3. **B 路线未取到 CE 子树可视帧**：CE 提交的子树挂在 `gui_root_block()` 上；进入 GUI 的唯一入口 `gui_begin()` 会 `gui_reset()→tree_reset()` 清空块表，CE 应用自身也无进入 GUI 帧循环的槽位。ISO-only 启动又无 FAT 卷可落内置应用。**归因：环境/内核入口限制；本路线证据以提交 verdict + 字段级缺口清单为准。**
4. **emit-c 角色名缺陷（工具侧，已绕过未改工具）**：`blk2bin --emit-c` 把 fill/fg/accent 的角色名以引号字符串写进 `uint32_t fg/bg/accent` 字段（见 `run/sample.c`：`.bg = "panel"`），不能直接编译；角色应进 `role_*` 枚举。本证据链用只读辅助脚本 `run/gen_blkgen.py` 直接消费 JSON 生成正确的 `blkgen.c`，未改动 `tools/blkhtml/*`。**归因：工具缺陷，需主代理决定是否回改 blk2bin.py。**
5. **容器 kind 与 flow 口径**：fixture 节点 2 `kind="column"` 但 `layout.flow="row"`；内核主轴按 flow 取 BLK_ROW 才与预览 rect 一致。生成器对容器优先采用 flow。**归因：fixture 标签滞后于 flow，映射策略在辅助脚本内固化。**

## ⑥ 需要主代理拍板的事

1. 是否回修 `blk2bin.py --emit-c`：角色名应输出 `role_*` 枚举而非字符串字面量（当前产物不可直接编译）。
2. CE 通道是否需要扩种类（BUTTON/INPUT/TERM/FREE）或在记录结构里补颜色/布局字段；当前缺口是 ABI 设计边界。
3. 是否需要为 CE 提交子树提供一个「不重置树」的只读查看入口，以便后续 B 路线能取可视对照帧。
4. 另一子代理的 `samples/*.blk.json` 第二组对照本次未跑（时间盒）；是否需要补跑同一套 A/B 链路。
5. CE 源文件命名：构建约定要求 `extapp/<app>.c`（apps.list 项 `blkgen` → `extapp/blkgen.c`），与任务书写的 `extapp/blkgen_ce.c` 不一致；本次按构建约定落地为 `blkgen.c`。

## 附：改动文件（git status 口径）

- 新增：`kernel_v25/src/gui/blkgen.c`、`kernel_v25/extapp/blkgen.c`
- 修改：`kernel_v25/src/gui/chrome.c`（仅追加 blkgen 分支 + 抑制逻辑）、`kernel_v25/src/gui/gui_int.h`（仅加声明）、`kernel_v25/build.sh`（仅加 blkgen.o / blkgen.c）、`kernel_v25/build/apps.list`（加一行 `blkgen`）
- 未提交（no git commit）；未触碰禁写目录与契约文件。
