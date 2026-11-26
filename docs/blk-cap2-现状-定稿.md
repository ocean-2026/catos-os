# 块化 GUI cap2 现状定稿（v2.81 输入）

> 用途：作为 v2.81 对外承诺口径的输入底稿。本文**只读代码 + 只读既有门禁日志**核实，
> 未改任何源码、未 build、未跑 qemu。
>
> 核实基准：工作树 `/home/user/work/gui-blk2`，分支 `feat/blk-cap2`，tip `49334c3bb58cef823c5a012ecbd840362481eac8`；
> 门禁批次 `blkcap2-final2`（2026-11-26，Asia/Shanghai）。`gate-*-final3.log` 截至本文撰写时**不存在**（已列目录核实）。
>
> 字段/语义真源：`kernel_v25/include/gui/block.h`、`include/gui/tokens.h`、`include/ce.h`（`ce_blk_rec2_t` / `ce_blk_event2_t`）、`include/gui/decl.h`。
> HTML 命名权威：`gui-tools/docs/HTML-块树-转换规范.md` §4.12（26 个属性）+ §4.12.5（cap3 形态）。
>
> **状态口径（四态，逐条经代码核实消费点，不照抄 `blk-cap2-实现.md`）**
> - `已实现-有像素/行为证据`：渲染/布局/交互路径有读取该字段的消费点，且 final2 门禁串口行有取点或行为断言。
> - `已实现-仅结构镜像(无消费点)`：字段只在 `desc.c`（内置页镜像）/ `decl.c`（CE 镜像）之间拷贝，渲染/布局/交互路径无读取。
> - `探针覆盖但判据弱`：消费点存在，探针触达但断言只覆盖部分档位/角色/轴/缓动，或仅间接断言；含"消费点已核实但无任何探针触达"的条目（差距清单中单独注明）。
> - `未做`：无字段或无代码路径。
>
> 消费点判定：除 `desc.c`、`decl.c` 两处镜像拷贝外，在 `paint.c / textlayout.c / layout.c / popover.c / sb.c / icons.c / anim.c / shadows.c / compose.c / inputbox.c` 中读到该字段才算消费点。

---

## 1. 总表

### 1.1 §4.12 的 26 个属性

| # | HTML 属性 | 内核字段 | 语义/取值 | 内核消费点(file:行) | 探针 case | 门禁(日志) | 状态 |
|---|---|---|---|---|---|---|---|
| 1 | `data-alpha` | `alpha:u8` | 0=未声明(≡255)；1..255 块不透明度；子树乘法继承 | `paint.c:500-507` `blk_alpha_of`（父链相乘）；`paint.c:658/686` 取覆盖率；`anim.c:99/121/145` 动画插值；`textlayout.c:456-459` weak 副本 | `cap2-alpha64/128/192` | `gate-cap2-render-blkcap2-final2.log` | 已实现-有像素/行为证据 |
| 2 | `data-alpha-mode` | `alpha_mode:u8` | 0=ordered16（4×4 Bayer 16 档二值抖动）；1=none（四舍五入最近档，不抖动） | `paint.c:511-519` `alpha_cov_px`；`paint.c:663/686` 传入 mode | 无独立 case；alpha 探针未置 mode（恒 0），mode=1 路径未取点 | `gate-cap2-render-blkcap2-final2.log`（仅间接） | 探针覆盖但判据弱 |
| 3 | `data-elevation` | `elevation:u8` | 0..5 = flat/raised/hover/float/overlay/modal；光源固定右下；焦点自动 +1 上限 5 | `shadows.c:60` `sh_paint`；`shadows.c:66` 焦点 +1；`shadows.c:76/80` 取令牌强度与外扩；`anim.c:100/122/151` 整档切换 | `cap2-shadow-slices` | `gate-cap2-render-blkcap2-final2.log` | 已实现-有像素/行为证据 |
| 4 | `data-radius` | `corner_tl/corner_tr/corner_br/corner_bl:u8` | 单值四角同值；0..63px；128=pill；255=auto；0=inherit(不显式) | `paint.c:557-564` `corner_raw`；`paint.c:580-609` `resolve_corner`；`paint.c:611-618` `cap2_resolve`；`paint.c:640-653` 掩码合成 | `cap2-corner-per` / `corner-pill` / `corner-concentric` | `gate-cap2-render-blkcap2-final2.log` | 已实现-有像素/行为证据 |
| 5 | `data-radius-tl` | `corner_tl:u8` | 覆盖左上角；值域同 `data-radius` | `paint.c:559`（case 0）；同 #4 路径 | `cap2-corner-per`（tl=8） | `gate-cap2-render-blkcap2-final2.log` | 已实现-有像素/行为证据 |
| 6 | `data-radius-tr` | `corner_tr:u8` | 覆盖右上角 | `paint.c:560`（case 1）；同 #4 路径 | `cap2-corner-per`（tr=16） | `gate-cap2-render-blkcap2-final2.log` | 已实现-有像素/行为证据 |
| 7 | `data-radius-br` | `corner_br:u8` | 覆盖右下角 | `paint.c:562`（default→br）；同 #4 路径 | `cap2-corner-per`（br=24） | `gate-cap2-render-blkcap2-final2.log` | 已实现-有像素/行为证据 |
| 8 | `data-radius-bl` | `corner_bl:u8` | 覆盖左下角 | `paint.c:561`（case 2）；同 #4 路径 | `cap2-corner-per`（bl=4） | `gate-cap2-render-blkcap2-final2.log` | 已实现-有像素/行为证据 |
| 9 | `data-border-sides` | `border_sides:u8` | bit0 l / bit1 r / bit2 t / bit3 b；0=全边（旧行为） | **无消费点**。仅 `desc.c:60/79`、`decl.c:476` 镜像拷贝；边框路径 `paint.c:702-712` `body_border_cap2` 不读该字段 | 无 | — | 已实现-仅结构镜像(无消费点) |
| 10 | `data-wrap` | `wrap_lines:u8` | 0=不折行；1..6=固定行数；255=auto(≤6) | `layout.c:40/45-46` 自然高；`textlayout.c:145` 行数钳制；`textlayout.c:289/424` 排版 | `cap2-wrap1` / `wrap2` / `wrapauto` | `gate-cap2-text-blkcap2-final2.log` | 已实现-有像素/行为证据 |
| 11 | `data-ellipsis` | `ellipsis:u8` | 0=无；1=字符 +"…"；2=词边界 | `textlayout.c:161` 词边界分支；`textlayout.c:174/178` 省略号追加；`textlayout.c:424` 传入 | `cap2-ellipsis-char` / `ellipsis-word` | `gate-cap2-text-blkcap2-final2.log` | 已实现-有像素/行为证据 |
| 12 | `data-font` | `font_size:u8` | 0=16(默认)；1=8；2=16；3=24；4=32 | `textlayout.c:27-33` `SLOT[5]`；`textlayout.c:35-43` `slot_idx`；`textlayout.c:263-265` `txt_font_h`；`layout.c:16/42/45` 度量 | `cap2-font8` / `font16` / `font24` / `font32`（+`font8-cjk`） | `gate-cap2-text-blkcap2-final2.log` | 已实现-有像素/行为证据 |
| 13 | `data-bold` | `font_bold:u8` | 0/1；8px 档 8×16 位图，其余档 1px 横向膨胀伪粗 | `textlayout.c:437` `draw_line` 传 bold；`font8.c:38` `f8_ascii_bold` | `cap2-bold` | `gate-cap2-text-blkcap2-final2.log` | 已实现-有像素/行为证据 |
| 14 | `data-tabular` | `font_tabular:u8` | 0=比例数字；1=等宽数字（时钟跳秒不抖） | `textlayout.c:119` 数字/冒号等宽步进；`textlayout.c:273/424/437` 传入 | `cap2-tabular` | `gate-cap2-text-blkcap2-final2.log` | 已实现-有像素/行为证据 |
| 15 | `data-line-h` | `line_h:u8` | 0=令牌默认行高；>0 显式 px 行高 | `textlayout.c:255-258` `line_height`；`textlayout.c:292/427` 传入 | `cap2-cache`（h3=40 显式行高，断言 h3≠h1、hits=1）；无独立行高取点 case | `gate-cap2-text-blkcap2-final2.log` | 探针覆盖但判据弱 |
| 16 | `data-anim` | `anim_ms:u16` | 0=无动画；档值 48/96/144/192/288（声明档；`anim.c` 未做档值校验钳制） | `anim.c:86` 零开销判断；`anim.c:93` 建档；`anim.c:109` 累计；`anim.c:129` 帧比 | `cap2-anim-outback` / `anim-interrupt` / `anim-reduced`（均 192ms） | `gate-cap2-interact-blkcap2-final2.log` | 已实现-有像素/行为证据 |
| 17 | `data-ease` | `anim_ease:u8` | 0=none；1 linear；2 in；3 out；4 inout；5 out-back；6 in-back；7 step | `anim.c:86/95` 建档；`anim.c:132` 帧表取值；`anim.c:38/51-58` `ease_tab[8][19]` | `anim-outback`(ease=5)、`anim-interrupt`/`anim-reduced`(ease=1)；其余 5 种缓动未单独取点 | `gate-cap2-interact-blkcap2-final2.log` | 探针覆盖但判据弱 |
| 18 | `data-anim-props` | `anim_mask:u8` | bit0 x / bit1 y / bit2 w / bit3 h / bit4 alpha / bit5 elevation | `anim.c:96` 建档；`anim.c:141-145` x/y/w/h/alpha 插值；`anim.c:146-151` elevation 整档切换 | 三 case 均 `mask=1`（仅 x）；y/w/h/alpha/elevation 掩码未单独取点 | `gate-cap2-interact-blkcap2-final2.log` | 探针覆盖但判据弱 |
| 19 | `<c-popover>` `data-anchor` | `anchor_id:u32` | 锚块 id；0=父框；引用解析失败按规则拒绝（不静默落 0） | `popover.c:69-77` `anchor_rect`；`decl.c:299-304` 同批 local id 解析；`decl.c:484-495` 归属校验 | `cap2-ce-popover-anchor`（legal=1 badid=2）；`pop-down/up/left/right` | `gate-cap2-ce-blkcap2-final2.log`；`gate-cap2-interact-blkcap2-final2.log` | 已实现-有像素/行为证据 |
| 20 | `<c-popover>` `data-prefer` | `pop_prefer:u8` | up1 / down2 / left4 / right8，可位或；0=down|right | `popover.c:111-114` 首选位序；`popover.c:115-120` 默认序去重补齐 | `cap2-pop-down/up/left/right` | `gate-cap2-interact-blkcap2-final2.log` | 已实现-有像素/行为证据 |
| 21 | `<c-popover>` `data-max-h` | `pop_max_h:u16` | 0=不限；>0 最大高 px，超出内部滚动 | `popover.c:105` 高度钳制；`popover.c:157-163` 超高转 OVF_SCROLL | `cap2-pop-scroll` | `gate-cap2-interact-blkcap2-final2.log` | 已实现-有像素/行为证据 |
| 22 | `data-icon` | `icon:u8`（kind=BLK_ICON） | 内置图标号 0..255，内核实装 0..7；缺省 0 | `icons.c:71-77` 取号/缩放；`icons.c:79-91` 逐像素绘制；`paint.c:848` 分派 | `cap2-icon-tint`（icon=7 close 单点）；`cap2-perf` 遍历 0..7 但无逐枚断言 | `gate-cap2-render-blkcap2-final2.log` | 已实现-有像素/行为证据 |
| 23 | `data-tint` | `tint_role:u8` | 0=不自着色(走 fg)；1 fg；2 bg；3 accent；4 border | `icons.c:63-69` 角色选色 | `cap2-icon-tint` 取证 role 3 accent / 4 border / 1 fg；role 2 bg 未取证 | `gate-cap2-render-blkcap2-final2.log` | 探针覆盖但判据弱 |
| 24 | `data-scrollbar` | `scrollbar:u8` | 0=auto；1=always；2=never | `sb.c:43-48` `sb_visible`（never→0 / always→1 / auto 按溢出） | `cap2-sb-paint`（scrollbar=1 always）；auto / never 两态未单独取点 | `gate-cap2-render-blkcap2-final2.log` | 探针覆盖但判据弱 |
| 25 | `data-scroll` | `scroll_axis:u8` | 0=auto(按溢出轴)；1=x；2=y；3=both | `sb.c:27-41` `need_x/need_y`；`sb.c:105/113` 滚轮分轴；`popover.c:159` 浮层内部 y 轴 | `cap2-sb-paint`(axis=2)、`cap2-pop-scroll`(axis=2)；x 轴 / both 未单独取点 | `gate-cap2-render-blkcap2-final2.log`；`gate-cap2-interact-blkcap2-final2.log` | 探针覆盖但判据弱 |
| 26 | `data-theme="hc"` | `hc_force:u8` | 1=本子树按 HC 规则渲染（radius=0 / border=2，阴影关断）；只影响本子树 | `shadows.c:69-73` HC 阴影→描边替代；`paint.c:548-550` `hc_forced`；`paint.c:581` 半径归零 | **无 cap2 探针**；final2 像素组仅 app / calm，无 HC 组；`hc_force=1` 块级覆盖未取点 | — | 探针覆盖但判据弱（消费点已核实，无探针触达） |

### 1.2 kind 15 / 16

| # | HTML 标签 | 内核 kind | 语义/取值 | 内核消费点(file:行) | 探针 case | 门禁(日志) | 状态 |
|---|---|---|---|---|---|---|---|
| 27 | `<c-popover>` | `BLK_POPOVER=15` | 锚定浮层：两趟 measure→place/翻转/钳制/内部滚动 | `layout.c:325` 调 `pop_layout`；`popover.c:80` `pop_layout`；`paint.c:842` 分派；`decl.c:231` CE 白名单 | `pop-down/up/left/right/clamp/scroll/dismiss/bridge`（8 例） | `gate-cap2-interact-blkcap2-final2.log` | 已实现-有像素/行为证据 |
| 28 | `<c-icon>` / 带 `data-icon` 元素 | `BLK_ICON=16` | 内置单色图标：icon 号 + tint 角色着色 | `paint.c:848` 分派；`icons.c:59` `icon_draw`；`decl.c:231` CE 白名单 | `cap2-icon-tint` | `gate-cap2-render-blkcap2-final2.log` | 已实现-有像素/行为证据 |

### 1.3 cap3 角形态（§4.12.5）

| # | HTML 属性 | 内核字段 | 语义/取值 | 内核消费点(file:行) | 探针 case | 门禁(日志) | 状态 |
|---|---|---|---|---|---|---|---|
| 29 | `data-radius-shape` | `corner_shape:u8` | 0=ARC(默认)；1=SQUIRCLE；`g2`=squircle 别名 | `paint.c:319-320` 形态判别；`paint.c:305-311` `cap3_ensure` 建表；`paint.c:649-652` 掩码取表 | `shape-arc` / `shape-sq5` / `shape-sq45` / `shape-diff` / `shape-seam` / `shape-lazy` | `gate-cap2-render-blkcap2-final2.log` | 已实现-有像素/行为证据 |
| 30 | `data-squircle-n` | `corner_n:u8` | 0=默认 50(5.0)；单位 0.1；仅 SQUIRCLE 有效；越界钳 50 + `WARN_RADIUS_N_RANGE` | `paint.c:321-329` n 判别（仅认 0/45/50，其余钳 50 并告警去重） | `shape-n2`（回归 diff=0）；`shape-clamp`（warn=1 sq5=1 sq45=0） | `gate-cap2-render-blkcap2-final2.log` | 已实现-有像素/行为证据 |

### 1.4 CE 通道

| # | 能力 | 内核结构/槽位 | 语义/取值 | 内核消费点(file:行) | 探针 case | 门禁(日志) | 状态 |
|---|---|---|---|---|---|---|---|
| 31 | 能力位探测 | `decl_caps()` / `DECL_CAP2_ALL` | 9 位：ALPHA/RADIUS/ELEV/WRAP/FONT/ANIM/SCROLLBAR/BUTTON/EVENTS2；`caps=0x1ff` | `decl.c:586` `return DECL_CAP2_ALL`；`decl.h:72-86` 位定义 | `cap2-ce-caps`；abi `caps mask want 0x181` | `gate-cap2-ce-blkcap2-final2.log`；`gate-cap2-abi-blkcap2-final2.log` | 已实现-有像素/行为证据 |
| 32 | rec2 记录提交/校验 | `ce_blk_rec2_t`（前 40B=rec1；`cap_ver=1`） | 旧 40B 路径不变；整批校验，非法整批拒绝、不触碰旧子树 | `decl.c:237-259` `validate2`；`decl.c:457-481` 扩展段镜像；`ce.h:103-131` 结构 | `cap2-ce-rec1-compat`；abi `rc2=0` / `badrec=-39` / `rec1=0` | `gate-cap2-ce-blkcap2-final2.log`；`gate-cap2-abi-blkcap2-final2.log` | 已实现-有像素/行为证据 |
| 33 | event2 事件回传 | `ce_blk_event2_t`（前 20B=旧事件） | ev_kind 0 click / 1 focus / 2 blur / 3 input / 4 wheel / 5 key / 6 dismiss；含 target_id/mods/wheel/utext/seq | `decl.c:677-703` 类型→ev_kind 映射；`decl.c:750-894` 各类事件投递（wheel `decl.c:861-863`、dismiss `decl.c:887-894`） | `cap2-ce-button`(kind 0)、`cap2-ce-input`(kind 3)；wheel/key/focus/blur/dismiss 未经 CE E2E 取证 | `gate-cap2-ce-blkcap2-final2.log` | 探针覆盖但判据弱 |
| 34 | CE kind 扩权 | rec2 白名单 | 旧白名单 COLUMN..CANVAS + BUTTON/INPUT/TERM/POPOVER/ICON；CE 仍禁写像素 | `decl.c:229-234` `kind_allowed_rec2`；`decl.c:257-259` 内联像素拒绝 | `ce-button` / `ce-input` / `ce-popover-anchor`；`BLK_TERM` kind 未在探针中提交取证 | `gate-cap2-ce-blkcap2-final2.log` | 探针覆盖但判据弱 |
| 35 | ring3 槽位 77/78/79 | `SYS_DECL_CAPS` / `COMMIT2` / `EVENT_POLL2` | 槽位可用；rec2/76B 提交；非法 `rec_size=77`→`-39`；40B 旧路径 `rec1=0` | `decl.h:87-89` 入口声明；`decl.c:237-259` 校验；abi 驱动 4 断言 | abi 4 项断言 PASS | `gate-cap2-abi-blkcap2-final2.log` | 已实现-有像素/行为证据（备注：ring3 事件 E2E 为已知遗留，见 §4） |

**总表条数：35**（26 属性 + 2 kind + 2 cap3 + 5 CE）。

---

## 2. 每档实测数字（数字均来自代码或日志实测；核不到的标"未取证"）

### 2.1 alpha（不透明度）
- 抖动档：ordered16 共 **16 档**（4×4 Bayer，`level = alpha*15/255`，`paint.c:517-519`）；mode=1 none 走最近档量化（`paint.c:513-515`）。
- 子树继承：父链 alpha 相乘（`paint.c:502-505`）。
- 取证：alpha64 `cov_maxerr=1`；alpha128 `cov_maxerr=2`；alpha192 `cov_maxerr=2`（容忍一档抖动噪声 ≈17/255，`cap2p_render.c:127`）。**仅 3 个绝对值取点；16 档全档逐档未取证。**
- mode=1（none）：**未取证**。

### 2.2 elevation（阴影）
- **6 档** 0..5；外扩半径 `SH_PAD[6]={0,2,3,5,7,10}`，右下偏移 `SH_OX/SH_OY[6]={0,1,2,3,4,6}`（`shadows.c:21-23`）。
- 焦点自动 +1 档，上限 5（`shadows.c:66`）；HC 下阴影关断、改描边（`shadows.c:69-73`）。
- 9-slice：6 档 × 10×10 角掩码 + 6 档 × 10 边缘剖面，静态 ≈0.7 KiB（`shadows.c:8/25-26`）。
- 取证：`present=5 seam_bad=0`（6 档中 flat 档不画投影，5 档有投影；拼缝坏 0）。

### 2.3 corner（圆角）
- 逐角值域 0..63px；`pill=128`（半高/半宽取整，上限 64）；`auto=255`（同心圆角，子半径=父半径−padding，仅 calm 模式生效，`paint.c:589-604`）；贴边归零兜底（`paint.c:581/614-617`）。
- 掩码分档桶 **8 档** `{4,8,12,16,24,32,48,64}`（`paint.c:524-525`）；calm 阶梯 **4 档** `{8,12,16,24}`（`paint.c:221-222`）。
- 取证：`corner-per cut=1 body=1`（tl=8/tr=16/br=24/bl=4）；`corner-pill outer_cut=1 body=1`；`corner-concentric child_body=1 parent_body=1`。

### 2.4 wrap / ellipsis（折行与省略）
- wrap：0 不折行；**1..6** 固定行数；**255=auto(≤6)**；行数钳制 `wrap>6→6`（`textlayout.c:145`）；单行上限 `MAX_LINE=8`（`textlayout.c:55`）。
- 取证：`wrap1 lines=1 ell=1 w=56`；`wrap2 lines=2 w0=80 w1=56`；`wrapauto lines=6 maxw=32`。
- 省略：`ellipsis-char lines=1 ell=1 w=32 full=344`；`ellipsis-word lines=1 ell=1 boundary=1`。
- 行首禁则：`kinsoku lines=6 bad=0`。

### 2.5 字号 / 伪粗 / 等宽
- 四档槽位（`textlayout.c:27-33`）：slot 0→16（aw8/ww16）、1→8（aw8/ww8）、2→16（aw8/ww16）、3→24（aw12/ww24）、4→32（aw16/ww32）。
- 取证：`font8 h=8 aw=8 ww=8`；`font16 h=16 aw=8 ww=16`；`font24 h=24 aw=12 ww=24`；`font32 h=32 aw=16 ww=32`。
- 伪粗：`bold thin=148 bold=208`（像素数对比）；8px 档伪粗产出 8×16 位图（`font8.c:38`）。
- 等宽数字：`tabular w00=40 w01=40 digit=8`。
- 8px CJK：`font8-cjk lit=22 ink16=48 left=14 right=8`（2×2 覆盖率加权降采样，非独立 CJK 字模）。
- 缓存：`cache h1=36 h2=36 h3=80 h4=36 hits=1`（h3 为 line_h=40 命中未中缓存路径）。

### 2.6 动画（缓动 / 时长 / 掩码）
- 缓动 **7 种**：1 linear / 2 in / 3 out / 4 inout / 5 out-back / 6 in-back / 7 step；0=none（`anim.c:51-58`，`ease_tab[8][19]`）。
- 时长档声明 48/96/144/192/288 ms（`block.h:156`）；**代码未做档值校验/钳制**，`anim.c` 直接以 `anim_ms` 为帧比分母。
- 掩码 **6 属性**：bit0 x / bit1 y / bit2 w / bit3 h / bit4 alpha / bit5 elevation（`anim.c:141-151`）；elevation 整档切换、过半切目标档（`anim.c:147-150`）。
- 槽位 **16**（`ANIM_SLOTS=16`，`anim.c:20`）；帧表上限 **19** 帧（`ANIM_MAX_IDX=18`）。
- 取证：`anim-outback last=120 max=132 over=1 target=120 frames=28,53,91,104,122,130,131,132,129,126,122,120`（12 帧，峰值 132 → 落点 120）；`anim-interrupt x=160 target=160 busy=0`；`anim-reduced x=100 target=100 busy=0`。
- 未取证：除 out-back/linear 外的 5 种缓动；除 x 外的 5 个掩码位；48/96/144/288 三档时长。

### 2.7 popover（锚定浮层）
- 四向位：up1 / down2 / left4 / right8；间隙 `POP_GAP=8`；安全边距 `POP_SAFE=8`（`popover.c:22-30`）。
- 内部滚动：超高转 `OVF_SCROLL` + `scroll_axis=2(y)`，滚动上限按内容高−视口高（`popover.c:157-163`）。
- 关闭：点最上层浮层外 → `decl_post_dismiss`(ev_kind=6)；ESC 同路径（`popover.c:188-193` `pop_esc`）。
- 斜向 hover 桥：`BRIDGE_TICKS=40` @100Hz = **400ms**（`popover.c:24/196-202`）。
- 取证（串口实测坐标）：
  - `pop-down anchor=100,100,80,40 pop=104,148,72,16 gap=8`
  - `pop-up anchor=100,300,80,40 pop=104,276,72,16 gap=8`
  - `pop-left anchor=400,100,80,40 pop=320,112,72,16 gap=8`
  - `pop-right anchor=100,100,80,40 pop=188,112,72,16 gap=8`
  - `pop-clamp pop=688,15,64,16`（不出屏）
  - `pop-scroll content_h=160 max_scroll=100 scroll_y=0 ovf=2 axis=2`
  - `pop-dismiss alive=0 count=1 did=3`（**仅点外关闭取证；ESC 关闭路径无探针**）
  - `pop-bridge kept=1 cnt1=0 closed=1 cnt2=1`（桥内不关、桥过期后关）

### 2.8 scrollbar（滚动条）
- 轨道宽 `SB_W=6`；thumb 最小长 16px；滚轮一次 `3 × line_h`（`sb.c:13-14/57/70/102-103`）。
- 可见三态：auto / always / never（`sb.c:45-47`）；轴 auto / x / y / both（`sb.c:29-39`）。
- 交互：滚轮 `sb_wheel`（`sb.c:98-122`）；拖 thumb `sb_mouse` press/drag/release（`sb.c:130-189`）。
- 取证：`sb-paint thumb=1 track=1`。**auto/never 态、x/both 轴、拖 thumb 行为未单独取点。**

### 2.9 icon（内置图标）
- **8 枚**（0 menu / 1 search / 2 settings / 3 file / 4 folder / 5 warn / 6 info / 7 close），每枚 **16×16** 位图，8×16×16bit=256B 静态（`icons.c:16-57/8`）。
- 缩放：块 ≥24×24 时 2× 放大到 32 档（`icons.c:73-74`）。
- tint 四角色：1 fg / 2 bg / 3 accent / 4 border；0 走传入 fg（`icons.c:63-69`）。
- 取证：`icon-tint accent=1 border=1 fg=1 act0=5017087 act1=5593702 act2=15263980 want1=5593702 want2=15263980`（仅 #7 close、3 角色）。**其余 7 枚图标与 bg 角色未逐枚取点。**

### 2.10 cap3 形态表
- **三套表**：ARC（n=2.0，n10=20）/ SQ5（n=5.0，n10=50）/ SQ45（n=4.5，n10=45）（`paint.c:226-230`）。
- 每套 **4 档半径** × **24×24** 面积采样（`paint.c:221-222/232`）；惰性建表，首帧用到才分配（`paint.c:305-311`）。
- 取证：`shape-arc maxerr=0`；`shape-sq5 cut=1 body=1`；`shape-sq45 cut=1 body=1`；`shape-n2 diff=0 cells=1040`；`shape-diff maxdiff=255`；`shape-seam top=1 left=1 seam=1`；`shape-clamp warn=1 sq5=1 sq45=0`；`shape-lazy mask1=3 mask2=7`。

### 2.11 令牌新增档位（tokens.h）
- `radius4[4]` 缺省角档；`shadow[6]` 各 elevation 强度；`spacing`；`font_small/body/title/display`（8/16/24/32）；`line_h=18`；`radius_hc_zero`；`border_w_hc=2`（`tokens.h:35-44`，`tokens.c:13-40` 三主题取值）。
- 注：CE `ce_blk_rec2_t` **不含** `hc_force` 与 `corner_n` 字段（`ce.h:127` 注释；`decl.c:407`），这两项仅内置页全字段路径可声明。

---

## 3. 证据索引（gate 日志 + 串口关键行，照抄原文）

### 3.1 批次总览（`gate-sum-blkcap2-final2.log`）
- 工作树 `/home/user/work/gui-blk2`，tip `49334c3bb58cef823c5a012ecbd840362481eac8`，ISO 12,163,072 字节（sha256 前缀 `6c9ba19568e73c95`），`_bss_end=0x02121f94`，arena `[0x02000000, 0x02800000)`。
- 汇总行（照抄）：
  - `1  build.sh iso                RC=0,0 error    PASS RC=0,0 error          --      57.5s       gate-build-blkcap2-final2.log`
  - `2  check_link_layout --strict  RC=0            PASS RC=0                  --      0.2s        gate-linklayout-blkcap2-final2.log`
  - `3  vprobe                      10/10           PASS 10/10 fail=0          4463    162.5s      gate-vprobe-blkcap2-final2.log`
  - `4  E3                          2/2             PASS 2/2 fail=0            4462    80.4s       gate-e3-blkcap2-final2.log`
  - `5  F11                         9/9             PASS 9/9 fail=0            4485    162.1s      gate-f11-blkcap2-final2.log`
  - `6a pixel style-app             3/3             PASS 3/3 checks passed     4486    43.1s       gate-pixel-app-blkcap2-final2.log`
  - `6b pixel style-calm            3/3             PASS 3/3 checks passed     4487    43.0s       gate-pixel-calm-blkcap2-final2.log`
  - `7a cap2_render                 18/18           PASS 18/18 passed          4491    309.2s      gate-cap2-render-blkcap2-final2.log`
  - `7b cap2_text                   14/14           PASS 14/14 fail=0          4492    133.4s      gate-cap2-text-blkcap2-final2.log`
  - `7c cap2_ce                     5/5             PASS 5/5 passed            4493    107.3s      gate-cap2-ce-blkcap2-final2.log`
  - `7d cap2_interact               11/11           PASS 11/11 fail=0         4494    119.0s      gate-cap2-interact-blkcap2-final2.log`
  - `7e cap2_abi                    4 项断言 PASS   PASS 4/4 断言              4496    70.2s       gate-cap2-abi-blkcap2-final2.log`
  - `7f run_decl                    基线同态        见下 (cvok FAIL 为既有环境问题) 4499-4503 266.2s  gate-decl-blkcap2-final2.log`

### 3.2 渲染包（`gate-cap2-render-blkcap2-final2.log`，18/18，串口 `scratch/cap2-render-final2.serial.log`）
- `GUI:cap2-alpha64 PASS cov_maxerr=1 result=1`
- `GUI:cap2-alpha128 PASS cov_maxerr=2 result=1`
- `GUI:cap2-alpha192 PASS cov_maxerr=2 result=1`
- `GUI:cap2-shadow-slices PASS present=5 seam_bad=0`
- `GUI:cap2-corner-per PASS cut=1 body=1`
- `GUI:cap2-corner-pill PASS outer_cut=1 body=1`
- `GUI:cap2-corner-concentric PASS child_body=1 parent_body=1`
- `GUI:cap2-icon-tint PASS accent=1 border=1 fg=1 act0=5017087 act1=5593702 act2=15263980 want1=5593702 want2=15263980`
- `GUI:cap2-sb-paint PASS thumb=1 track=1`
- `GUI:cap2-perf PASS full_us=2113 idle_us=29 area=0`
- `GUI:cap2-shape-arc PASS maxerr=0`
- `GUI:cap2-shape-sq5 PASS cut=1 body=1`
- `GUI:cap2-shape-n2 PASS diff=0 cells=1040`
- `GUI:cap2-shape-diff PASS maxdiff=255`
- `GUI:cap2-shape-seam PASS top=1 left=1 seam=1`
- `GUI:cap2-shape-clamp PASS warn=1 sq5=1 sq45=0`
- `GUI:cap2-shape-lazy PASS mask1=3 mask2=7`
- `GUI:cap2-shape-sq45 PASS cut=1 body=1`
- 驱动汇总：`cap2-render: 18/18 passed`

### 3.3 文本包（`gate-cap2-text-blkcap2-final2.log`，14/14，串口 `scratch/cap2-text-final2.serial.log`）
- `GUI:cap2-wrap1 PASS lines=1 ell=1 w=56`
- `GUI:cap2-wrap2 PASS lines=2 w0=80 w1=56`
- `GUI:cap2-wrapauto PASS lines=6 maxw=32`
- `GUI:cap2-ellipsis-char PASS lines=1 ell=1 w=32 full=344`
- `GUI:cap2-ellipsis-word PASS lines=1 ell=1 boundary=1`
- `GUI:cap2-font8 PASS h=8 aw=8 ww=8`
- `GUI:cap2-font8-cjk PASS lit=22 ink16=48 left=14 right=8`
- `GUI:cap2-font16 PASS h=16 aw=8 ww=16`
- `GUI:cap2-font24 PASS h=24 aw=12 ww=24`
- `GUI:cap2-font32 PASS h=32 aw=16 ww=32`
- `GUI:cap2-bold PASS thin=148 bold=208`
- `GUI:cap2-tabular PASS w00=40 w01=40 digit=8`
- `GUI:cap2-kinsoku PASS lines=6 bad=0`
- `GUI:cap2-cache PASS h1=36 h2=36 h3=80 h4=36 hits=1`
- 驱动汇总：`steps=14 ok=14 fail=0 elapsed=133.4s`

### 3.4 CE 通道包（`gate-cap2-ce-blkcap2-final2.log`，5/5，串口 `scratch/cap2-ce-final2.serial.log`）
- `GUI:cap2-ce-caps PASS caps=0x000001ff`
- `GUI:ev click id=2 x=3 y=5` / `GUI:cap2-ce-button PASS id=2 x=3 y=5`
- `GUI:ev input id=2 len=11` / `GUI:ev input id=2 len=21` / `GUI:cap2-ce-input PASS bytes=11 clip=21`
- `GUI:cap2-ce-popover-anchor PASS legal=1 badid=2`
- `GUI:cap2-ce-rec1-compat PASS rec1=2 rec2=3 rec1b=2`
- 驱动汇总：`cap2-ce: 5/5 passed`

### 3.5 交互包（`gate-cap2-interact-blkcap2-final2.log`，11/11，串口 `scratch/cap2-interact-final2.serial.log`）
- `GUI:cap2-pop-down PASS anchor=100,100,80,40 pop=104,148,72,16 gap=8`
- `GUI:cap2-pop-up PASS anchor=100,300,80,40 pop=104,276,72,16 gap=8`
- `GUI:cap2-pop-left PASS anchor=400,100,80,40 pop=320,112,72,16 gap=8`
- `GUI:cap2-pop-right PASS anchor=100,100,80,40 pop=188,112,72,16 gap=8`
- `GUI:cap2-pop-clamp PASS pop=688,15,64,16`
- `GUI:cap2-pop-scroll PASS content_h=160 max_scroll=100 scroll_y=0 ovf=2 axis=2`
- `GUI:cap2-pop-dismiss PASS alive=0 count=1 did=3`
- `GUI:cap2-pop-bridge PASS kept=1 cnt1=0 closed=1 cnt2=1`
- `GUI:cap2-anim-outback PASS last=120 max=132 over=1 target=120 frames=28,53,91,104,122,130,131,132,129,126,122,120`
- `GUI:cap2-anim-interrupt PASS x=160 target=160 busy=0`
- `GUI:cap2-anim-reduced PASS x=100 target=100 busy=0`
- 驱动汇总：`steps=11 ok=11 fail=0 elapsed=119.0s`

### 3.6 ABI / 槽位（`gate-cap2-abi-blkcap2-final2.log`，4/4 断言）
- 驱动断言：
  - `PASS  caps mask              want 0x181`
  - `PASS  rec2 commit rc2==0     rc2=0`
  - `PASS  illegal rec_size -> E_DECL_BADREC  badrec=-39`
  - `PASS  legacy rec1 rec1==0    rec1=0`
- 应用自裁行（已知遗留，不计本门禁失败，照抄）：`caps=0x000001ff rc2=0 badrec=-39 click=0 id=0 x=0 y=0 input=0 text= rec1=0 app=FAIL`
- 串口：`GUI:decl commit n=3 ok=1 pid=1` / `GUI:decl reject code=-39 pid=1` / `GUI:decl toast code=-39 pid=1` / `GUI:decl commit n=3 ok=1 pid=1` / `GUI:decl leaked=0`

### 3.7 基线回归（非 cap2 专项，用于同态核对）
- vprobe（`gate-vprobe-blkcap2-final2.log`）：`steps=10 ok=10 fail=0 elapsed=162.5s`（fr/cat/hw/str/strcpy/wr/fw/pw 等通用 CE 探针，非 cap2 断言）。
- E3（`gate-e3-blkcap2-final2.log`）：`PASS badptr` / `PASS conc`，`steps=2 ok=2 fail=0 elapsed=80.4s`。
- F11（`gate-f11-blkcap2-final2.log`）：9 条 PASS（passwd-alice / passwd-bob / root-lsuser-tags / login alice / alice edit -c / alice lsuser / login root / root edit -c passwd 等）。
- 像素 app（`gate-pixel-app-blkcap2-final2.log`）：`== 3/3 checks passed ==`，点 `(65,65)` / `(224,164)` / `(400,585)`。
- 像素 calm（`gate-pixel-calm-blkcap2-final2.log`）：`== 3/3 checks passed ==`，点 `(344,26)` / `(224,164)` / `(400,585)`。
- run_decl（`gate-decl-blkcap2-final2.log`）：`counters: {"FAIL": 1, "SKIP": 3, "PASS": 1}`；cvok 像素断言 FAIL（`rect=(158,104,200,160) count=0 want >=20 ~= (232,232,236)`）为既有环境/基线同态问题，重跑复现，非本包回归；cvdeny/deny_gfx/cvleak SKIP（NOT-SUPPORTED）；ctrl_images `records=1` PASS。
- 构建（`gate-build-blkcap2-final2.log`）：`OK -> /home/user/work/gui-blk2/kernel_v25/catos.iso (12163072 bytes)`，0 error（仅基点既有 warning）。
- 链接（`gate-linklayout-blkcap2-final2.log`）：`[link] _bss_end = 0x02121f94  arena window = [0x02000000, 0x02800000)`，`--strict` RC=0。

### 3.8 补充证据（final2 之后的 deskev 波次，另一工作树 `gui-blk2-deskev`，不计入 final2 12 项）
- 日志：`gate-cap2-deskev-blkcap2-deskev.log`。串口行（照抄）：
  - `GUI:deskev pid=1 id=1 kind=1 x=0 y=0 w=80 h=24`
  - `GUI:deskev pid=1 id=2 kind=1 x=0 y=0 w=80 h=24`
  - `GUI:deskev pid=1 id=3 kind=12 x=0 y=0 w=16 h=8`
  - `GUI:deskev pid=1 id=4 kind=13 x=0 y=8 w=80 h=16`
  - `GUI:ev click id=2 x=8 y=4`
  - `GUI:ev click id=3 x=40 y=8`
  - `GUI:ev input id=3 len=5`
- 说明：该波次解决 `gui_reset()->tree_reset()` 清空 CE 子树导致 ring3 点击/输入事件无块可命中的断链；实现文档记探针 6/6（`click=1 id=2 x=8 y=4`、`input=1 text=catos`）。**该证据不在 final2 批次内，v2.81 引用时须注明波次。**

---

## 4. 差距清单（状态非"已实现-有像素/行为证据"）

| # | 能力 | 为什么不是证据态 | 建议（补齐 / 降级承诺 / 注明限制） |
|---|---|---|---|
| G1 | `data-border-sides`（#9） | **无消费点**：字段仅 `desc.c:60/79`、`decl.c:476` 镜像，边框绘制 `body_border_cap2`（`paint.c:702-712`）不读它；声明单边/双边描边当前不产生任何像素差异。 | v2.81 **明确不承诺**单边描边；若要保留字段，需在 `body_border*` 路径按 bit0..bit3 逐边裁剪描边并补探针；否则在 HTML 规范标注"结构预留、暂不生效"。 |
| G2 | `data-alpha-mode` mode=1（#2） | 消费点在（`paint.c:511-519`），但全部 alpha 探针恒走默认 mode=0；none（不抖动）路径无取点对比。 | 补一组 mode=0/mode=1 同 alpha 并排取点；补齐前对外只承诺 ordered16 抖动。 |
| G3 | `data-line-h`（#15） | 仅在 `cap2-cache` 内间接断言（h3≠h1），无独立行高取点 case；显式行高与令牌行高的换算未直接核对。 | 补独立 `line-h` case：同一文本分别取令牌默认 / 显式 px，核对行高差与总行数；补齐前标"实验性"。 |
| G4 | `data-ease` 7 缓动（#17） | 帧表 7 种全在代码内，但探针只取证 ease=5（out-back）与 ease=1（linear）；in/out/inout/in-back/step 五种曲线未取点。 | 补 5 种缓动的帧表落点/峰值取点；补齐前对外只承诺 out-back 与 linear。 |
| G5 | `data-anim-props` 6 掩码（#18） | 三例探针均 `mask=1`（仅 x）；y/w/h/alpha/elevation 五个掩码位未单独驱动与断言。 | 按位补 case（至少 y、w、alpha、elevation 各一）；补齐前承诺"仅位 x 有动画证据"。 |
| G6 | `data-tint` bg 角色（#23） | 取证 role 3 accent / 4 border / 1 fg；role 2 bg 未取点；0 号"不自着色走 fg"也未单独断言。 | 补 bg 角色取点；补齐前承诺 fg/border/accent 三角色。 |
| G7 | `data-scrollbar` auto/never（#24） | 消费点三态齐全（`sb.c:45-47`），探针只置 always=1；auto 按溢出出现、never 隐藏两态未核对。 | 补 auto（有/无溢出两子态）与 never 取点；补齐前承诺 always 绘制。 |
| G8 | `data-scroll` x / both 轴（#25） | 探针只测 y 轴（axis=2）；x 轴与 both 双轴未取点；拖 thumb 行为也未单独断言。 | 补 x/both 轴与拖 thumb case；补齐前承诺纵向滚动条。 |
| G9 | `data-theme="hc"` `hc_force`（#26） | 消费点在（`shadows.c:69-73`、`paint.c:548-550/581`），但无 cap2 探针置 `hc_force=1` 取点；final2 像素组无 HC 组。块级 HC 覆盖（区别于切换 HC 主题）的降级效果未取证。 | 补 `hc_force=1` 子树取点（radius=0、border=2、阴影关断）；补齐前 HC 能力只承诺"跟随 HC 主题令牌"，不承诺块级覆盖。 |
| G10 | event2 wheel/key/focus/blur/dismiss（#33） | 投递函数齐全（`decl.c:750-894`），CE E2E 仅取证 click(kind0) 与 input(kind3)；wheel/key/focus/blur/dismiss 未经 ring3 往返核对。dismiss 在内核探针侧由 `pop-dismiss` 覆盖（owner=0 不走 CE 队列）。 | 补 CE 侧各类事件往返探针；补齐前事件承诺限于 click/input。 |
| G11 | CE `BLK_TERM` kind（#34） | 白名单含 TERM（`decl.c:231`），探针未提交 TERM 记录；放行是否正确、块内终端在内核树如何落地未取证。 | 补 TERM 提交/校验 case；补齐前 CE kind 承诺 BUTTON/INPUT/POPOVER/ICON。 |
| G12 | popover ESC 关闭（附属于 #27） | `pop_esc()` 路径存在（`popover.c:188-193`），`pop-dismiss` 探针只模拟点外关闭；ESC 按键关闭无探针。 | 补 ESC 注入关闭 case；补齐前关闭承诺限于"点外关闭 + 400ms 桥"。 |
| G13 | ring3 事件 E2E 遗留（附属于 #35） | final2 abi 应用自裁 `click=0 input=0`：`gui_reset()->tree_reset()` 进桌面清空 CE 子树，桌面内真实点击/键盘在块表中无块命中。deskev 波次在另一工作树以保留模式修复并有串口证据（§3.8），**未回流进 final2 基线**。 | v2.81 须等 deskev 改动回流主树并复跑 final2 全量门禁后，方可承诺桌面内 CE 事件往返；在此之前 ring3 承诺限于槽位/提交/校验路径。 |
| G14 | 8 枚图标逐枚像素（附属于 #22） | 8 枚位图均在代码内（`icons.c:16-57`），像素断言仅 #7 close；其余 7 枚无逐枚取点。 | 补 0..7 逐枚中心像素/轮廓断言；补齐前不逐枚承诺图标造型。 |

---

## 5. v2.81 输入建议（三档）

### 5.1 可以对外承诺（有 final2 门禁像素/行为证据）
1. 块级不透明度：`alpha` 子树乘法继承，ordered16 抖动合成；取证 64/128/192 三档，覆盖率误差 ≤2/255。
2. 阴影 6 档 elevation（flat/raised/hover/float/overlay/modal），9-slice 右下光源，焦点自动 +1；拼缝坏 0。
3. 逐角圆角 0..63px + pill + auto（同心，calm 模式）；四向逐角、胶囊、父子同心均有取点。
4. 文本折行 1/2/auto(≤6)；字符省略 / 词边界省略；行首禁则 0 违规。
5. 四档字号 8/16/24/32；伪粗（像素数 148→208）；等宽数字。
6. 动画：192ms out-back 过冲曲线（峰值 132→落点 120，12 帧）；可中断接管；reduced-motion 一帧直达。
7. 锚定浮层：四向放置、8px 间隙与安全边距、出屏钳制、超 `max_h` 内部纵向滚动、点外关闭、400ms 斜向 hover 桥。
8. 标准滚动条：track + thumb 绘制，纵向滚轮滚动（内核统一路径）。
9. 图标：内置单色图标块，fg / border / accent 三角色着色（close 图标单点取证）。
10. cap3 角形态：ARC / SQ5 / SQ45 三套 4 档半径 ×24×24 采样表；n 越界钳 50 并告警；惰性建表。
11. CE 通道：能力位 `caps=0x1ff`；rec2 提交/整批校验/rec1 40B 兼容；非法 `rec_size`→`-39`；click / input 两类事件内核侧往返。
12. 新 kind 落地：`BLK_POPOVER(15)`、`BLK_ICON(16)` 进入布局/绘制/CE 白名单。
13. 基线同态：vprobe 10/10、E3 2/2、F11 9/9、像素 app 3/3 + calm 3/3、build RC=0 0 error、`--strict` RC=0。

### 5.2 只能写"实验性"（消费点存在，取证不完整）
1. `alpha_mode=none`（不抖动量化）。
2. 显式 `line_h` 行高。
3. 除 out-back / linear 外的 5 种缓动；除 x 外的 y/w/h/alpha/elevation 动画掩码；48/96/144/288 时长档。
4. `tint_role=bg`（角色 2）与 role 0 不自着色。
5. 滚动条 auto / never 态；x 轴 / both 双轴；拖 thumb。
6. 块级 `hc_force` HC 覆盖（radius=0 / border=2 / 阴影关断）。
7. event2 的 wheel / key / focus / blur / dismiss 事件；CE `BLK_TERM` kind。
8. 8 枚内置图标的逐枚造型（仅 #7 取证）；8px CJK 降采样渲染。
9. 桌面内 ring3 真实点击/键盘事件往返（待 deskev 改动回流主树并复跑门禁）。

### 5.3 明确不承诺
1. `data-border-sides` 单边/双边/三边描边：当前无消费点，声明不生效（G1）。
2. CE 通道下发 `hc_force` 与 `corner_n`：`ce_blk_rec2_t` 结构未含这两项字段（`ce.h:127`、`decl.c:407`），CE 应用无法经 rec2 声明。
3. CE 应用内联像素写入：校验路径一律拒绝（`decl.c:257-259`，`E_PIX_FIELD`），CE 仍禁写像素。
4. 独立 8px CJK 字模：当前为 16×16 汉字模 2×2 覆盖率降采样，不承诺 8px 档 CJK 字形质量。
5. 任何未经 §3 门禁串口行取点的档位/角色/轴/缓动，不在 v2.81 承诺口径内。

---

## 附：核实范围与未取证项说明
- 本文消费点逐条在 `kernel_v25/src/gui/` 下核实；`docs/blk-cap2-实现.md` 仅作线索，未照抄其状态结论。
- 未跑 build / qemu / 链接布局检查；所有运行期数字引自 final2 门禁日志串口行。
- `gate-*-final3.log` 截至撰写时不存在；若后续出现，需回填 §3 并复核本定稿状态。
- 未取证字段/档位已在 §2 各小节与 §4 差距清单中标注，未做估算填充。
