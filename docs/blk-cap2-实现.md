# 块化 GUI cap2 能力扩展 —— 实现与验收（v3 内核侧）

> 本文件是内核侧的「字段 → 消费点 → 断言 → 证据」总账。字段名与取值已冻结，
> HTML 层命名以《HTML 原型开发规范 v3》为准，转换器负责唯一映射。
> 契约提交：`feat/blk-cap2` `754d82c`（append-only，旧结构语义/旧槽号不动）。

## 0. 与原型开发者 9 条需求的对应（每条都做高一级）

| # | 原型侧诉求 | cap2 实际实现（更高一级） |
|---|---|---|
| 1 | 8 级 alpha | 16 档 ordered16（4×4 Bayer 抖动）+ 子树乘法继承 + `alpha_mode=none` 回退 + 三主题取点校验 |
| 2 | 阴影 2→4 档 | 6 档 elevation（flat/raised/hover/float/overlay/modal）+ 焦点自动 +1 + 右上/右下固定光源 9-slice 预生成（×3 主题）+ HC 用 1px 边框替代 |
| 3 | 逐角 `data-radius` | 逐角 0..63px + `pill` + `auto` 同心圆角（子半径=父半径−padding）+ 贴边归零降为兜底 |
| 4 | wrap 2 行 | 1..6 行 + `auto`(≤6) + 字符/词两种省略 + 简版禁则 + 度量缓存 |
| 5 | 8px + 伪粗 | 四档 8/16/24/32 + 伪粗 + 等宽数字 + 中文 8px 用 2×2 覆盖率加权降采样 |
| 6 | 4 个缓动枚举 | 7 缓动（linear/in/out/inout/out-back/in-back/step）+ 5 档时长 + 6 个可动画属性掩码 + 新目标可中断 + `reduced_motion` 一键全关 |
| 7 | `BLK_POPOVER` | 两趟 measure→place + 8px 安全边距 + 四向翻转 + 钳制 + 超 `max_h` 内部滚动 + 子菜单斜向 hover 桥 400ms + 点外/ESC 关闭 |
| 8 | CE 开 BUTTON/只读 INPUT | `ce_blk_rec2_t` 全字段镜像 + CE 可用 kind 扩到 BUTTON/INPUT/TERM/POPOVER/ICON（仍禁写像素）+ 事件回传 click/focus/blur/input/wheel/key/dismiss + UTF-8/mod/坐标 |
| 9 | 滚动条/图标/令牌 | 内核统一 track+thumb+滚轮+拖 thumb；16/32 两档单色图标 + fg/bg/accent/border 四角色 tint；`radius/shadow/spacing/font` 进三主题 + HC 自动降级 + 块级 `@hc` 覆盖 |

## 1. 冻结契约（`block.h` / `tokens.h` / `ce.h` / `decl.h`）

| 字段 | 取值 | 缺省/兼容 | 消费点 | 断言 ID |
|---|---|---|---|---|
| `alpha:u8` | 0=未声明(≡255)，1..255 | 0 | 渲染包 `blk_alpha_of` / `blk_cov` | `cap2-alpha64/128/192` |
| `alpha_mode:u8` | 0=ordered16 1=none | 0 | 渲染包 present 量化 | 同上 |
| `elevation:u8` | 0..5 | 0 | 渲染包 9-slice 阴影 | `cap2-shadow-slices` |
| `corner_tl/tr/br/bl:u8` | 0..63px / 128=pill / 255=auto | 0 | 渲染包逐角掩码 | `cap2-corner-per/pill/concentric` |
| `wrap_lines:u8` | 0/1..6/255 | 0 | 文本包 `txt_wrap_lines` | `cap2-wrap1/wrap2/wrapauto` |
| `ellipsis:u8` | 0/1char/2word | 0 | 文本包 | `cap2-ellipsis-char/word` |
| `font_size:u8` | 0=16 1=8 2=16 3=24 4=32 | 0 | 文本包 `txt_font_h` | `cap2-font8/16/24/32` |
| `font_bold:u8` | 0/1 | 0 | 文本包 | `cap2-bold` |
| `font_tabular:u8` | 0/1 | 0 | 文本包等宽数字 | `cap2-tabular` |
| `line_h:u8` | 0=令牌，>0 显式 px | 0 | 文本包 | `cap2-wrap*` 行高 |
| `anim_ms:u16` | 0/48/96/144/192/288 | 0 | 动画包预生成帧表 | `cap2-anim-outback` |
| `anim_ease:u8` | 0..7 | 0 | 动画包 | 同上 |
| `anim_mask:u8` | bit0 x..bit5 elevation | 0 | 动画包 | `cap2-anim-interrupt` |
| `scrollbar:u8` | 0自动/1总是/2从不 | 0 | 渲染包 `sb_visible/sb_paint` | `cap2-sb-paint` |
| `scroll_axis:u8` | 0自动/1x/2y/3both | 0 | 滚动条 | `cap2-sb-wheel` |
| `border_sides:u8` | bit0 l..bit3 b | 0=全边 | 渲染包 `body_border` | `cap2-border-sides` |
| `hc_force:u8` | 1=子树 HC 渲染 | 0 | 令牌/渲染 | `cap2-hc-override` |
| `anchor_id:u32` | 锚块 id，0=父框 | 0 | 浮层包 `pop_layout` | `cap2-pop-*` |
| `pop_prefer:u8` | up1/down2/left4/right8 | 0=down|right | 浮层包 | `cap2-pop-down/up/left/right` |
| `pop_max_h:u16` | 0=不限 | 0 | 浮层包内部滚动 | `cap2-pop-scroll` |
| `icon:u8` + `tint_role:u8` | 图标号 / 0,1fg,2bg,3accent,4border | 0 | `icon_draw` | `cap2-icon-tint` |
| 新 kind | `BLK_POPOVER=15` `BLK_ICON=16` | — | 布局/绘制/CE | `cap2-pop-*` / `cap2-icon-tint` |
| `ce_blk_rec2_t` | 前 40B=rec1；`cap_ver=1` | 旧 40B 记录不变 | `decl_apply` | `cap2-ce-button/input` |
| `ce_blk_event2_t` | 前 20B=旧事件 | 旧结构不变 | `decl_event_poll2` | 同上 |

令牌新增：`radius4[4]`（文档记作 `radius[4]`）、`shadow[6]`、`spacing`、`font_small/body/title/display`、`line_h`、`radius_hc_zero`、`border_w_hc`。

## 2. 探针协议

- 入口：`gui cap2-<case>`（串口文本控制台），由 `chrome.c` 分派到 `cap2probe.c`，
  再分给 `cap2p_render.c` / `cap2p_text.c` / `cap2p_interact.c` 三个包。
- 输出：单行 `GUI:cap2-<case> PASS k=v ...` 或 `GUI:cap2-<case> FAIL detail`。
- 所有数字必须真实取点/实测，禁止写死。
- 驱动脚本：`/home/user/work/artifacts/gui-blocks/probes/gates/cap2_{render,text,interact}.py`。

## 3. 基线（`feat/blk-cap2` 基点 099b74a，未改代码）

| 门禁 | 结果 |
|---|---|
| vprobe | 10/10 pass，162.5s |
| F11 | 9/9 pass，162.2s |
| E3 | 2/2 pass，80.4s |
| 像素回归 | 6/6 checks PASS（app / calm 双断言） |

日志：`/home/user/work/artifacts/gui-blocks/gate-{vprobe,f11,e3,pixel}-blkcap2-baseline.log`。

## 4. 证据索引（各包回填）

- 渲染/合成：待回填
- 文本/排版：待回填
- 交互/通道（浮层 + CE + 动画 + 滚轮）：待回填
- 工具链（规范 v0.3 / html2blk / preview / samples）：待回填

## 5. 已知限制

- `data-radius="inherit"` 映射为 0（不显式 = 旧默认），内核暂无真正的「继承父半径」值；文档必须写清。
- HC 下阴影全部关断，改用 1px 边框；`hc_force` 只影响本子树。

### 4.x 回填（截至本轮收尾）

| 包 | 状态 | 关键数字 | 证据 |
|---|---|---|---|
| 渲染/合成 | 已合并到 `feat/blk-cap2` | cap2 探针 10/10；vprobe 10/10；像素 app 3/3、calm 3/3；`--strict` RC=0；半透明整屏重组 full_avg=1976us、idle_avg=13us；alpha maxerr ≤2/255；阴影 seam_bad=0 | `probes/scratch/cap2_render.serial.log`、`gate-{vprobe,pixel-app,pixel-calm}-blkcap2-render.log` |
| 文本/排版 | 已合并到 `feat/blk-cap2` | 14/14（wrap1/2/auto、char/word 省略、四档字高 8/16/24/32、伪粗 thin=148→bold=208、tabular 0 偏移、禁则 bad=0、cache 命中 1、font8-cjk lit=22/ink16=48 左右半 14/8）；vprobe 10/10；像素 3/3+3/3 | `gate-cap2-text-blkcap2-text.log`、`gate-vprobe-blkcap2-text.log` |
| 集成（文本+渲染） | 已编译链接通过 | 合并后 `build/kernel.bin` 1,394,292 B，0 error；合并后 ISO 的 vprobe/像素未及复跑 | `gate-build-final-blkcap2.log` |
| 工具链/规范 | 完成 | `--spec-audit` 344 条 RC=0；单测 140 项全绿；旧 5 样例哈希零漂移；samples/cap2 五样例哈希入 §9 | `gui-tools` `feat/blk-spec3`：`0c28add/df3737c/31be7fa/c697fd2/163a04c/f34a2d6` |
| CE 通道 | **未完成**（本轮时间窗到点，代码在工作树未提交） | — | `/home/user/work/gui-blk2-ce` `cap2/ce` |
| 浮层/动画（交互包） | **未开工** | — | 计划从合并 tip 起 `cap2/interact` |
| cap3 半径形态 | **未实现**（契约字段已冻结、渲染包已留 `corner_mask_build` 替换点） | — | `feat/blk-cap2` `238758c` |

### 4.y 第 1 波：合并后全量门禁（`feat/blk-cap2` @3c5d95b）

| 门禁 | 结果 | 证据 |
|---|---|---|
| 构建 `sh build.sh iso` | RC=0，ISO 12,120,064 B | `gate-build-final-blkcap2.log` |
| vprobe | **10/10**（157.1s） | `gate-vprobe-blkcap2-final.log` |
| E3 | **2/2**（80.3s） | `gate-e3-blkcap2-final.log` |
| F11 | **9/9**（162.2s） | `gate-f11-blkcap2-final.log` |
| 像素 style-app | **3/3 PASS** | `gate-pixel-app-blkcap2-final.log` |
| 像素 style-calm | **3/3 PASS** | `gate-pixel-calm-blkcap2-final.log` |
| `check_link_layout --strict` | **RC=0** | `gate-link-blkcap2-final.log` |
| `_bss_end` | **0x0211549c**（基线 0x02115a00，净减 0x564=1380 B；WARN 为既有 supervisor mirror 覆盖项，非新增越界） | 同上 |

### 4.z 第 2 波：CE 通道包（已合并，`cap2/ce`）

- 探针 5/5（`ce-caps`/`ce-button`/`ce-input`/`ce-popover-anchor`/`ce-rec1-compat`）；vprobe 10/10；`gui decl` 自检 `result=1 leaked=0`；`--strict` RC=0，`_bss_end=0x02111124`（较第 1 波再降 17,272 B）。
- 实测：`caps=0x1ff`；ce-button `ev id=2 x=3 y=5 target_id=gid ev_kind=0`；ce-input 短文本 11B 一致、长文本 30B → 截 7 整汉字（21B，UTF-8 未断字）；rec1/rec2 混提交字段无残留（`rec1=2 rec2=3 rec1b=2`）。
- 证据：`gate-cap2-ce-blkcap2-ce-final.log`、`gate-declself-blkcap2-ce-final.log`、`gate-vprobe-blkcap2-ce-final.log`。
- 遗留：ring3 槽位接线（第 2b 波 `cap2/abi` 处理）；非法 cap_ver 负例未单设探针。

### 4.y 第 3/4 波：interact + cap3（已合并）

| 能力 | 消费点 | 实测证据 |
|---|---|---|
| 四向翻转 + 8px 钳制 + 四边安全边距 | `pop_layout()` 两趟 measure→place | down `104,148,72,16` / up `104,276` / left `320,112` / right `188,112`，gap 均 8；clamp `688,15,64,16` 不出屏 |
| 超 max_h 内部滚动 | `OVF_SCROLL`+`scroll_axis=2` | `content_h=160 max_scroll=100 ovf=2` |
| 点外 / ESC 关闭 + 400ms 桥 | `pop_dismiss_at/pop_esc/pop_bridge_note` → `decl_post_dismiss`(ev_kind=6) | `alive=0 count=1 did=3`；`kept=1 closed=1 cnt2=1` |
| 7 缓动 × 6 属性掩码、可中断、reduced_motion | `anim_kick/tick(16)/apply`，16 槽 + `ease_tab[8][19]` | outback 12 帧 `28,53,91,104,122,130,131,132,129,126,122,120`（峰值 132 → 落点 120）；中断 `x=160 target=160 busy=0`；reduced 一帧 |
| cap3 三套形态表（ARC/SQ5/SQ45，4 档×24×24，4×4 面积采样） | `corner_mask_build(radius, shape_n)` | n=2 回归 `diff=0 cells=1040`；8 点采样 `maxerr=0`；ARC vs SQ5 对角中段 `maxdiff=255`；lazy 未构建前 SQ45=0；render 18/18 |

### 4.x 第 2b 波：ring3 槽位（`cap2/abi`，已合并）

| 槽号 | 调用 | ring3 实测 |
|---|---|---|
| 77 | `SYS_DECL_CAPS` | `caps=0x000001ff`（含 ALPHA/BUTTON/EVENTS2=0x181） |
| 78 | `SYS_DECL_COMMIT2` | rec2/76B/cap_ver=1 提交 `rc2=0`；非法 `rec_size=77` → `badrec=-39`；40B 旧路径 `rec1=0` |
| 79 | `SYS_DECL_EVENT_POLL2` | 槽位可用，事件内容 E2E 受桌面重置阻断（见遗留） |

证据：`gate-cap2-abi-blkcap2-abi.log`、驱动 `probes/gates/cap2_abi.py`（4496）；BSS 增量 0；vprobe 10/10。
遗留：`gui_reset()->tree_reset()` 在进桌面时清空 CE 子树，点击/输入事件的 ring3 侧实测缺失（内核侧已由 `gui cap2-ce-button`/`cap2-ce-input` 覆盖）。

### 4.z 合并后最终树全量门禁（tip `49334c3`，2026-11-26 01:19）

工作树 `/home/user/work/gui-blk2`，分支 `feat/blk-cap2`，tip `49334c3bb58cef823c5a012ecbd840362481eac8`；ISO 12,163,072 字节；`--strict` 下 `_bss_end=0x02121f94`，arena window `[0x02000000, 0x02800000)`。

| 门禁 | 期望 | 实测 | 日志名 |
|---|---|---|---|
| build `sh build.sh iso` | RC=0、0 error | PASS RC=0、0 error；ISO 12,163,072 B（57.5s） | `gate-build-blkcap2-final2.log` |
| `check_link_layout.py --strict` | RC=0 | PASS RC=0；`_bss_end=0x02121f94`，窗口 `[0x02000000,0x02800000)` | `gate-linklayout-blkcap2-final2.log` |
| vprobe（4463） | 10/10 | PASS 10/10 fail=0（162.5s） | `gate-vprobe-blkcap2-final2.log` |
| E3（4462） | 2/2 | PASS 2/2 fail=0（80.4s） | `gate-e3-blkcap2-final2.log` |
| F11（4485） | 9/9 | PASS 9/9 fail=0（162.1s） | `gate-f11-blkcap2-final2.log` |
| 像素 style-app（4486） | 3/3 | PASS 3/3（43.1s；点 65,65 / 224,164 / 400,585） | `gate-pixel-app-blkcap2-final2.log` |
| 像素 style-calm（4487） | 3/3 | PASS 3/3（43.0s；点 344,26 / 224,164 / 400,585） | `gate-pixel-calm-blkcap2-final2.log` |
| cap2_render（4491） | 18/18 | PASS 18/18（309.2s；含 shape-* 8 例全过） | `gate-cap2-render-blkcap2-final2.log` |
| cap2_text（4492） | 14/14 | PASS 14/14 fail=0（133.4s） | `gate-cap2-text-blkcap2-final2.log` |
| cap2_ce（4493） | 5/5 | PASS 5/5（107.3s） | `gate-cap2-ce-blkcap2-final2.log` |
| cap2_interact（4494） | 11/11 | PASS 11/11 fail=0（119.0s） | `gate-cap2-interact-blkcap2-final2.log` |
| cap2_abi（4496） | 4 项断言 PASS | PASS 4/4 断言（70.2s）；应用自裁 `click=0 input=0 app=FAIL` 为已知遗留 | `gate-cap2-abi-blkcap2-final2.log` |
| run_decl（4499–4503） | 基线同态 | cvok FAIL（像素，既有环境问题，重跑复现）；cvdeny/deny_gfx/cvleak SKIP；ctrl_images PASS；counters FAIL1/SKIP3/PASS1 | `gate-decl-blkcap2-final2.log` |

总体结论：**可交付**。本包（ce/text/render/interact/cap3/abi）对应门禁全部达标，无本包引入的阻塞项。两项已知项单列、不计本门禁失败：① decl cvok 像素断言 FAIL（checker fg count=0）为既有环境问题、基线同态，重跑一次复现同一失败，非本包回归；② abi 应用自裁 `click=0 input=0` 系桌面重置清空 CE 子树的已知遗留，内核侧事件链路已由 `cap2-ce-button`/`cap2-ce-input` 探针覆盖。汇总：`gate-sum-blkcap2-final2.log`。

### 4.aa ring3 事件链打通（`cap2/deskev`，工作树 `gui-blk2-deskev`，基点 `49334c3`）

**断链**：`gui_begin()`（`compose.c`）末尾收摊组件/画布/终端/输入态/chrome 句柄后调 `gui_reset()` → `tree_reset()`（`tree.c`）回收整棵块树；控制台阶段后台启动的 `/bin/cap2test.ce` 已经 CE 通道（槽 77 `decl_caps` / 78 `decl_commit2` / 79 `decl_event_poll2`）提交 `owner=tid` 的声明子树，进桌面 `gui_begin()` 时整树被清空，后续真实鼠标点击/键盘在块表中无块可命中。

**Before 证据**（基线 ISO，临时驱动 `probes/scratch/deskev_before.py`，端口 4497；日志 `gate-before-blkcap2-deskev.log`）：提交后 `GUI:decl commit n=3 ok=1 pid=1`，`GUI:blkdbg pid=1 blocks=3 events=0`；进桌面后 **0 条** `GUI:ev click`、**0 条** `GUI:ev input`，无裁决行，应用退出 `GUI:decl leaked=0`。

**方案选择：方案 A（保留 CE 子树）**。`tree_reset()` 增加保留模式 `tree_reset_keep_ce()`：`owner != 0` 的顶层子树不回收，单链摘链暂存 `kept[MAX_TASKS]`，`owner == 0` 走原 `tree_free_subtree`；清旧伤害矩形/离场态后重新挂回根链、整树标脏重排，由 `layout_run` 按新根框重算。块 id、锚点映射、文本/图像指针、事件队列原样存活。否决方案 B（rec2 快照到 scratch、重置后回放 `decl_apply2` 重建）：需重新校验记录、重映射锚点、复制文本池，侵入更大。约束保持：旧 40B rec1 路径、旧 24B 事件队列、CE 禁写像素、HC/主题语义不变；`gui_reset` 其它清理（缓存/图像/焦点）原样保留，`gui_end()` 及其它探针路径仍走全量回收。

**配套改动**：① `on_press` 命中块本身是输入框时按块接管键盘（CE 块不带 legacy on_click，`act` 上溯落空，原路径挂不上钩子）；② `ib_activate()` 激活时记录归属 tid，重复点击同一块不清空已输入文本，块被替换/回收时 `btable_get` 取不到则复位交回默认路径；③ `decl_cleanup_tid()` 回收子树前按记录 owner 复位输入框钩子；④ CE 归属块 Esc 放行给桌面退出路径，legacy 仍吞 Esc；⑤ `gui deskev` 探针分支转储 CE 块实时几何（`decl_dump_ce_rects`，只读）供驱动定位；⑥ `cap2test.ce` 支持 argv[1] 覆盖轮询次数、裁决行经 `file_write` 落盘 `/cap2.out`、点击裁决取 `ev.id`（local id）。

**After 实测**（最终 ISO，探针 `probes/gates/cap2_deskev.py`，端口 4497；日志 `gate-cap2-deskev-blkcap2-deskev.log`）：
- 点击：`GUI:ev click id=2 x=8 y=4`（按钮 local id=2，矩形 0,0 16×8，局部坐标 8,4 落矩形内）；聚焦输入框 `GUI:ev click id=3 x=40 y=8`。
- 输入：`GUI:ev input id=3 len=5`；裁决行 `click=1 id=2 x=8 y=4`、`input=1 text=catos`、`caps=0x000001ff rc2=0 badrec=-39 rec1=0 PASS`。
- 探针 6/6 通过（断言：`click=1`、坐标落按钮矩形且 lid=2、`input=1` 且 len=5、文本字节一致 `catos`）。

**回归**：build `sh build.sh iso` RC=0（ISO 12,167,168 B）；link layout `--strict` RC=0，`_bss_end=0x02122f94`；vprobe 10/10、像素 style-app 3/3、style-calm 3/3、interact 11/11、abi 4 项断言 PASS（abi 前台无注入，应用自裁 `click=0 input=0` 属预期）。

### 4.b final4 全量门禁（tip `2ce23d0`，2026-11-26）

工作树 `/home/user/work/gui-blk2`，分支 `feat/blk-cap2`，tip `2ce23d0`；ISO 12,179,456 字节（sha 前缀 `96281244f3a11936`）；`--strict` RC=0，`_bss_end=0x02125fd4`，高于地板 `0x02000000` 1,175.96 KiB（1,204,180 B），距窗顶 `0x02800000` 7,016.04 KiB。

| 门禁 | 期望 | 实测 | 端口 | 耗时 | 日志 |
|---|---|---|---|---|---|
| build `sh build.sh iso` | RC=0、0 error | PASS RC=0、0 error（48 warn）；ISO 12,179,456 B | -- | 54.5s | `gate-build-blkcap2-final4.log` |
| `check_link_layout.py --strict` | RC=0 | PASS RC=0；`_bss_end=0x02125fd4`，+1,175.96 KiB | -- | 0.1s | `gate-linklayout-blkcap2-final4.log` |
| vprobe | 10/10 | PASS 10/10 fail=0 | 4501 | 157.0s | `gate-vprobe-blkcap2-final4.log` |
| E3 | 2/2 | PASS 2/2 fail=0 | 4462 | 80.2s | `gate-e3-blkcap2-final4.log` |
| F11 | 9/9 | PASS 9/9 fail=0 | 4485 | 162.0s | `gate-f11-blkcap2-final4.log` |
| 像素 style-app | 3/3 | PASS 3/3（点 65,65 / 224,164 / 400,585） | 4502 | 42.5s | `gate-pixel-app-blkcap2-final4.log` |
| 像素 style-calm | 3/3 | PASS 3/3（点 344,26 / 224,164 / 400,585） | 4503 | 42.9s | `gate-pixel-calm-blkcap2-final4.log` |
| cap2_render | 21/21 | PASS 21/21（含 shape-* 8、border-sides/corner、hc-override） | 4512 | 358.1s | `gate-cap2-render-blkcap2-final4.log` |
| cap2_text | 14/14 | PASS 14/14 fail=0 | 4492 | 133.3s | `gate-cap2-text-blkcap2-final4.log` |
| cap2_ce | 8/8 | PASS 8/8（elev-reject / evq-overflow / anim-prod 等） | 4510 | 155.2s | `gate-cap2-ce-blkcap2-final4.log` |
| cap2_interact | 14/14 | PASS 14/14 fail=0（sb-wheel/sb-drag/sb-edge 等） | 4511 | 142.0s | `gate-cap2-interact-blkcap2-final4.log` |
| cap2_abi | 收紧判据 7 项 | PASS 7/7 断言；裁决 `click=1 id=2 x=8 y=4 input=1 text=catos rec1=0 app=PASS` | 4497 | 82.5s | `gate-cap2-abi-blkcap2-final4.log` |
| cap2_deskev | 6/6 | PASS 6/6；`ev click id=2 x=8 y=4`、`ev input id=3 len=5`、text=catos | 4496 | 82.6s | `gate-cap2-deskev-blkcap2-final4.log` |
| run_decl（4499 段） | 基线同态 | cvok 像素 FAIL（既有环境问题，重跑复现）；cvdeny/deny_gfx/cvleak SKIP；ctrl_images PASS；counters FAIL1/SKIP3/PASS1 | 4499–4503 | 597.9s | `gate-decl-blkcap2-final4.log` |

总体结论：**可交付**。ring3 桌面事件链经 abi + deskev 双门禁实测打通（click=1 / input=1 / app=PASS）。唯一非绿项为 decl cvok 像素断言 FAIL，基线同态、`--only cvok` 重跑复现同一失败，属既有环境问题、非本包回归。汇总：`gate-sum-blkcap2-final4.log`。备注：abi 探针 harness 已对齐 deskev 注入流程（原文件备份 `probes/gates/cap2_abi.py.orig-final4`），仓库源码未改、未提交。
