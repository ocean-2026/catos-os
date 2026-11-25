# blkhtml 冻结契约（写在这里的事实，不要重新推导）

> 基线：`feat/blkhtml`，从 `main`=`89584c4` 开出；worktree `/home/user/work/gui-tools`。
> 只读参考：`/home/user/work/catos-os`（main 的只读镜像，等价于本 worktree）。
> **严禁进入** `/home/user/work/gui-hosted`、`/home/user/work/gui-fix`（并行线）。
> **不要动** `/home/user/work/gui-preview`（另一条未提交的预览线；可以读，不许写、不许复制成自己的交付）。
> 红线：不改 `kernel_v25/catos-v2.80*.iso` 与版本串；ISO/大二进制不进仓库根；CE 槽只 append（当前最后占用 = 76）；用户六份主文档只追加附录；`check_link_layout.py --strict` 必须 RC=0。

## 1. 块树的真实格式（不是 JSON）

运行时是**扁平二进制记录数组 + 字符串池**，经 CE 槽 72 提交：

```c
/* kernel_v25/include/ce_abi.h:85-98 */
typedef struct {
    int32_t id; int32_t parent; uint8_t kind; uint8_t pad0; uint16_t layer;
    int32_t req_w, req_h;
    int32_t text_off; int32_t pix_off; int32_t pix_w, pix_h; int32_t res;
} ce_blk_rec_t;                     /* sizeof = 40 字节/条，无内部空洞 */
```

> **步长以 `sizeof` 为准（40，不是 36）**：`decl.c:329-330` 按 `n * sizeof(rec_scratch[0])` 拷贝，
> 字段偏移 `id@0 parent@4 kind@8 pad0@9 layer@10 req_w@12 req_h@16 text_off@20 pix_off@24 pix_w@28 pix_h@32 res@36`。
> 早期版本误记 36（漏 `pix_off`），已修正；`pix_off` 必须写在记录里，值恒为 `-1`（`decl.c validate` 拒绝内联像素）。

提交语义（`kernel_v25/src/gui/decl.c:163-360` `decl_commit` / `validate` / `decl_apply`）：

- `text_off == -1` 表示无文本；否则必须 `0 <= text_off < slen`，否则 `E_DECL_BADREC(-39)`。
- `layer` **只接受 200（LAYER_WORK）**；`chrome/overlay/toast` 调 `blk_commit` 会被 `E_DECL_LAYER(-36)` 整批拒绝。
- 配额：256 块、深度 16、文本 4096B、池 32768B（`gui/decl.h`）。
- 整批校验、非法整批拒绝、不触碰旧子树；内核不保存用户指针。

**结论（这决定全部设计）**：`ce_blk_rec_t` 里**没有几何、没有 padding/gap/align、没有 anchor/pct、没有 overflow、没有颜色、没有 radius/shadow**。
所以 `.blk.json` 必须是「ce 记录镜像 + 声明字段（超集）」两层：

| 层 | 字段 | 去哪 |
|---|---|---|
| `ce` | id/parent/kind/layer/req_w/req_h/text/pix/res | `blk2bin` → `recs.bin`+`strings.bin` → 槽 72 |
| 声明超集 | rect/padding/gap/layout/anchor/px/pct/overflow/drag/resize/bleed/fill/stroke/radius/shadow/font/z | 只进 `blk2bin --emit-c` 的内核内置页（`blk_spec` 全字段），**不能**经槽 72 |

## 2. 内核实际具备的视觉能力（去掉了幻想）

`graphics` 在 89584c4 的真实情况（逐行核对过源码）：

- `blk_spec`（`include/gui/block.h`）**确实有** `overflow/anchor/pct_*/px_*/min_*/max_*/m_*/drag_*/resize_*/scroll_*`，`layout.c:230-280` 的 `layout_free()` **确实实现了** ANC_L/R/HCENTER/V… 与 `pct_x/pct_y` 求解；`grid_snap()` 只把 `padding/gap` 归整到 `{4,8,12,16,24}`（`layout.c:34-35`），**不对坐标做 8pt 对齐**。
- 圆角：**只有 calm 档、只有尺寸阶梯**（`paint.c:196-260`）——`CALM_RADII = {8,12,16,24}` 按 `min(w,h)` 分档，超椭圆 n≈5 离线掩码逐像素填；`corner_mode==1` 强制直角；`tokens.h` 的 `corner_radius` 字段**没有消费者**。→ **没有逐角半径，没有贴边方角归零。**
- 阴影：`paint.c:296-315` `paint_halo()`——按 layer 决定：WORK 只画接触层（1px 黑 18% @ `y+1`），OVERLAY/TOAST 画双层（`y+1`、`y+3`、`y+4`）。→ **固定四边，没有「贴边侧不投影」。**
- 分隔线：**没有**父子共享边只画 1px 的合成逻辑；父描边 + 子描边会各画各的。
- `calm_shape_eligible()` 排除：`BLK_ROOT`、`bleed`、`LAYER_CHROME`、`LAYER_BG`、`corner_mode==1`、高对比度令牌。

**已知口径偏差（先记下来，别当 bug 查）**：
1. `grid_snap()` 只在 `gui_style_calm()` 为真时生效（`layout.c:35` 第一行提前返回），且它把 `padding/gap` 向上吸到 `{4,8,12,16,24}`——所以 `padding=2` 在 calm 档会变成 4，app 档原样 2。样例请只用 `{0,4,8,12,16,24}`。
2. calm 档下 `BLK_BUTTON/BLK_INPUT` 的自然高度被抬到最小 24（`layout.c:53-58`），文本块不抬。
3. `BLK_DIVIDER` 自然高度恒 1px，`BLK_SPACER` 取 `req_h`。

**`docs/块化GUI-v2-规格.md` 在 89584c4 上并没有「贴边方角」与「圆角覆盖率」条目**（全仓 grep 只命中 `docs/night-review-aesthetic.md` 等旧文；规格文档第 134 行反而写着「不做…圆角」）。
→ 本工具按 `schema.json :: flush_rule` **在设计层实现**该规则（`radius`/`stroke.sides`/`shadow.sides` 归零、共享边 1px），并在预览 `design` 模式体现；内核侧做不到，作为 **ABI/实现缺口**如实上报，**不许自己往 ABI 里加字段**。

## 3. 三条预览口径（必须逐像素对齐内核）

| 模式 | 画什么 | 对应内核状态 |
|---|---|---|
| `app`（默认） | 直角、1px 描边、无阴影 | `/etc/style` = `app`（`calm_shape_eligible` 全 false） |
| `calm` | 尺寸阶梯超椭圆圆角 + `paint_halo` 的层决定双层阴影 | `/etc/style` = `calm` |
| `design` | `.blk.json` 逐角 `radius` + `shadow.sides`，并执行 `flush_rule` | **目标态**，内核暂不具备 |

QEMU 截图对照只允许拿同口径比：`app`↔style=app，`calm`↔style=calm。`design` 只做「设计 vs 内核」差异清单，不冒充同布局。

## 4. 令牌与字模（预览的唯一来源）

- 颜色：`schema.json :: token_palette`，逐字取自 `kernel_v25/src/gui/tokens.c:10-23`（dark/light/hc）。**禁止自拟配色。**
- 字模：`kernel_v25/src/font_ascii.inc`（IBM VGA 8x16，95 字形，16 字节/字形，MSB 左）。预览必须解析它来画字，不许用系统字体近似。
- 文本度量：ASCII 8px、双字节 16px、`scale=2` 各乘 2；与 `decl.c:632 decl_text_width` 一致。

## 5. 工程事实（省得重查）

- 构建：`sh build.sh iso`（root shim）→ `kernel_v25/build/kernel.bin` + `kernel_v25/catos.iso`；CE 应用 `sh kernel_v25/extapp/build_ce.sh <app>.c`，清单 `kernel_v25/build/apps.list`（被 `build/gen_builtin.py` 用来嵌入 `/bin/*.ce`）。
- CE 提交范例：`kernel_v25/extapp/dektest.c`（树 3 条 + 池 "Hi" + 串口判据）。
- 内核内置页 / 探针入口：`kernel_v25/src/gui/chrome.c:1704 gui_shell_cmd()`，shell 命令 `gui [probe]`（`src/shell.c:352,7348`）。新增页=在 `gui_shell_cmd` 里追加一个分支（append-only，不动枚举）。
- QEMU：`qemu-system-i386`（`/home/user/.local/bin` 与 `/home/user/bin` 均在 PATH）；HMP 截图驱动参考 `tests/qemu_regress.py` + `tests/qemu_driver.py`（`sendkey` + `screendump`，端口按套件固定，**按 PID 杀进程**，别 `pkill -f qemu`）。
- 护栏：`python3 kernel_v25/tools/check_link_layout.py <kernel.pe> --strict`（要传 PE 路径；不传会 `nm` 报错）。

## 6. cap2 / cap3 冻结字段映射（v0.3，append-only）

> 命名纪律：**HTML 属性名以《HTML 原型开发规范 v3》为权威**，内核字段名以本表冻结值为准；两者映射唯一，不在工具线侧另立命名。完整五列总账见 `docs/HTML-块树-转换规范.md` §4.12 / §4.12.5；`schema.json :: cap2/cap3` 与 `blkcommon.py :: CAP2_FIELDS / CAP3_FIELDS` 同表镜像，审计 A-9-05..10 复核。

### 6.1 cap2 字段冻结表

| 内核字段 | 类型 | 取值 / 默认 | HTML 属性 |
|---|---|---|---|
| `alpha` | u8 | 0=未声明(≡255)；0–1 比例 / 0–255 绝对值 | `data-alpha` |
| `alpha_mode` | u8 | 0=ordered16 / 1=none，默认 0 | `data-alpha-mode` |
| `elevation` | u8 | 0..5（flat/raised/hover/float/overlay/modal），默认 0 | `data-elevation` |
| `corner_tl/tr/br/bl` | u8 | 0..63px / 128=pill / 255=auto / 0=inherit | `data-radius`、`data-radius-tl/tr/br/bl` |
| `border_sides` | u8 | bit0 l / bit1 r / bit2 t / bit3 b；省略 0=全边 | `data-border-sides` |
| `wrap_lines` | u8 | 0 / 1..6 / 255=auto，默认 0 | `data-wrap` |
| `ellipsis` | u8 | 0=none / 1=char / 2=word，默认 0 | `data-ellipsis` |
| `font_size` | u8 | 1=8/caption 2=16/body 3=24/title 4=32/display；0=令牌默认 | `data-font` |
| `font_bold` | u8 | 0/1，默认 0 | `data-bold` |
| `font_tabular` | u8 | 0/1，默认 0 | `data-tabular` |
| `line_h` | u8 | 0=令牌默认；>0 px | `data-line-h` |
| `anim_ms` | u16 | 0=无动画；档 48/96/144/192/288 | `data-anim` |
| `anim_ease` | u8 | 0 none / 1 linear / 2 in / 3 out / 4 inout / 5 out-back / 6 in-back / 7 step | `data-ease` |
| `anim_mask` | u8 | bit0 x / bit1 y / bit2 w / bit3 h / bit4 alpha / bit5 elevation | `data-anim-props` |
| `anchor_id` | u32 | 0=父框；元素 id 解析结果 | `<c-popover> data-anchor` |
| `pop_prefer` | u8 | up1 / down2 / left4 / right8，可组合，默认 0 | `<c-popover> data-prefer` |
| `pop_max_h` | u16 | 0=不限 | `<c-popover> data-max-h` |
| `icon` | u8 | 0..255，默认 0；kind=BLK_ICON(16) | `data-icon` |
| `tint_role` | u8 | 0 未指定 / 1 fg / 2 bg / 3 accent / 4 border | `data-tint` |
| `scrollbar` | u8 | 0=auto / 1=always / 2=never | `data-scrollbar` |
| `scroll_axis` | u8 | 0=auto / 1 x / 2 y / 3 both | `data-scroll` |
| `hc_force` | u8 | 0/1，默认 0；1=本子树 HC（radius=0 / border=2） | `data-theme="hc"` |

### 6.2 cap3 形态字段冻结表

| 内核字段 | 类型 | 取值 / 默认 | HTML 属性 |
|---|---|---|---|
| `corner_shape` | u8 | 0=arc（默认）/ 1=squircle；`g2` 为 squircle 别名 | `data-radius-shape` |
| `corner_n` | u8 | 0=默认 50（5.0）；单位 0.1，仅 SQUIRCLE 有效；越界钳 50 + `WARN_RADIUS_N_RANGE`；非数值 `E_RADIUS_N_BAD` | `data-squircle-n` |

- 形态整块级别，与逐角 `corner_tl/tr/br/bl` 正交：形态整块统一（R-7.5-01），半径仍可逐角。
- CE 侧 `ce_blk_rec2_t` 只下发 `corner_shape`，**不下发** `corner_n`。
- 禁止 `data-radius="g2 16"` 这类关键字塞数值属性；形态只走 `data-radius-shape` / `data-squircle-n`。

## 7. 模块与命名（不要新增第 5 个脚本之外的入口）

```
tools/blkhtml/
├── schema.json      # 冻结契约（本文件配套，已写好，不改字段名）
├── html2blk.py      # HTML → .blk.json（含 diagnostics / UNSUPPORTED）
├── blk2bin.py       # .blk.json → recs.bin + strings.bin（+ --emit-c 内核内置页 C 片段）
├── preview.py       # .blk.json → 单文件 HTML / PNG（app|calm|design 三口径）
├── validate.py      # 断言 C1/G1/G2/B1/B2/R1/S1/S2/D1/T1/L1/O1/P1
├── samples/         # 5 个样例：.html + .blk.json + 预览产物
├── tests/           # 单元/回归夹具
└── README.md        # 用法 + 与 schema 的对应
```

纯 Python 3 标准库，**零第三方依赖**；PNG 由 `zlib`+`struct` 手写（不装 Pillow）。
所有脚本 `--help` 可用、非 0 退出码表示失败、诊断走 stderr 或 `--json`。

## 8. 提交与证据

- 分支 `feat/blkhtml`，工具 + 文档一起 commit；日志/截图落 `artifacts/gui-tools/`（**不进仓库根**）。
- 交付前必须：`validate.py` 全绿（5 样例）；`check_link_layout.py --strict` RC=0（若改了内核源）。
