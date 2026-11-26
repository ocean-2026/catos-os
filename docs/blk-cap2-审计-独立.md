# CatOS 内核 cap2 块化 GUI —— 只读独立审计报告

- 审计对象：内核树 `/home/user/work/gui-blk2`，分支 `feat/blk-cap2`，tip `079e699`（`merge cap2/deskev`）。
- 审计方式：从零读代码，不采信 `blk-cap2-实现.md` 的结论；只读，未 build / 未跑 qemu / 未 commit / 未改源码。
- 验收口径（三类）：① 文档说了但没实现；② 实现了但无消费点；③ 探针过了但判据是空。
- 证据口径：`文件:行号` 均相对 `kernel_v25/`；行号以 tip `079e699` 为准。

## 阻塞项

> 定义：会导致 CE 应用崩溃 / 事件丢失 / 像素错误的问题。

### B1〔高〕per-pid 事件队列满员即静默丢弃新事件，无报错、无计数、无覆盖

- 位置：`src/gui/decl.c:34`（`DECL_EVQ 8`）、`src/gui/decl.c:689-696`（`enqueue2`）。
- 证据原文：
  ```c
  static void enqueue2(struct pid_meta *p, ce_blk_event2_t *e) {
      int next = (p->evt + 1) % DECL_EVQ;
      if (next == p->evh) return;          /* 满：直接丢，无返回值/无计数 */
      e->seq = ++p->seq;
      p->evq[p->evt] = *e;
      p->evt = next;
      p->stat_events++;
  }
  ```
- 复现/判据：单 pid 连续投递第 9 个事件（队列深 8），`enqueue2` 静默 `return`；`stat_events` 不增、不覆盖旧事件、不向调用方回传错误；poll 侧与应用均无任何“丢了一条”的信号。`decl_post_click/input/key/dismiss` 等入口全部 `void` 落这里。
- 影响：突发事件（连续点击 + 焦点 + 输入 + 滚轮/关闭叠加）下事件丢失且不可感知，CE 应用状态可能与内核脱节。
- 建议修法：满队时递增一个溢出计数并经 `decl_dump_pid`/caps 暴露，或 `enqueue2` 返回满队标志供投递方降级；至少在文档写明“丢新不覆盖”。

### B2〔高〕`elevation > 5` 未钳制即进阴影表，越界读 `shadow[6]` / `SH_PAD[6]` 等 6 档数组

- 位置：`src/gui/decl.c:460`（rec2 原样镜像）、`src/gui/desc.c:65`（内置页原样镜像）、`src/gui/paint.c:791`（`sh_paint(b, b->elevation)` 入参无钳制）、`src/gui/shadows.c:65,76,80,86-87`。
- 证据原文：
  ```c
  int el = elevation;
  if (b->focused && el < 5) el++;        /* 只挡了焦点 +1 的上限，未挡入参 >5 */
  ...
  uint32_t str = tok->shadow[el];        /* tokens.h:36 shadow[6] */
  int pad = SH_PAD[el], ox = SH_OX[el], oy = SH_OY[el];   /* shadows.c:21 均 [6] */
  ```
- 复现/判据：`validate2`（`decl.c:237-310`）不校验 `elevation` 值域；CE rec2 或内置 spec 写 `elevation=6..255`，每帧 `sh_paint` 按下标 6..255 读 `tok->shadow[]`、`SH_PAD[]`、`SH_OX[]`、`SH_OY[]`、`sh_edge[el][]`、`sh_corner[el][][]`，越界读令牌结构/静态数组，轻则错误阴影像素，重则取到非法偏移触发页错误。
- 对照：焦点 +1 上限 5 在 `shadows.c:66` 有 `el < 5` 守卫，但声明值本身没有上界守卫。
- 建议修法：落地或绘制前钳制 `elevation = min(elevation, 5)`；`validate2` 对 `elevation>5` 报 `E_DECL_BADREC`。

---

## 一、文档说了但没实现（承诺字段/语义在内核找不到有效消费点）

### A1〔高〕`anim_kick` 无生产调用点：CE / 内置块声明的动画在真实帧路径永不启动

- 位置：定义 `src/gui/anim.c:83`；生产帧循环 `src/gui/compose.c:299`（`anim_tick`）、`compose.c:301`（`anim_apply_walk`）；字段镜像 `src/gui/decl.c:471-473`、`src/gui/desc.c:74-76`。
- 证据：全树检索 `anim_kick`，除定义、符号表外，仅探针 `src/gui/cap2p_interact.c:347,393,399,429` 调用；`compose.c / layout.c / decl.c / desc.c` 均无调用。
- 复现/判据：CE rec2 带 `anim_ms/anim_mask/anim_ease` 提交，或内置 spec 声明动画；帧循环只 `anim_tick` 推进 + `anim_apply` 按 id 找槽，而槽位从不建档（`find_slot(..., alloc=0)` 找不到即返回），块静止在布局值。
- 对照规范：§4.12 行 584-586（M-CAP2-13/14/15）；实现文档 `blk-cap2-实现.md:16,122` 称“7 缓动 × 6 属性掩码、可中断”，生产链路缺建档触发点。
- 建议修法：在 commit 落地 / 属性变更路径对声明动画的块调 `anim_kick(b)`，或在 `anim_apply_walk` 前对未建档的动画块补建档。

### A2〔高〕滚轮整条链路未接线：`dz` 已入队但 GUI 不分发，`sb_wheel`、`decl_post_wheel` 无生产调用

- 位置：`src/gui/sb.c:98`（`sb_wheel`）、`src/gui/decl.c:855`（`decl_post_wheel`）、`src/gui/gui_int.h:115`（声明）；输入分发 `src/gui/input.c:295-306`；事件结构 `include/mouse.h:38`（`dz` 滚轮位）。
- 证据：全树检索 `sb_wheel` / `decl_post_wheel`，除定义、头文件、符号表外无任何调用；`mouse.c:154-180` 已解析滚轮 `dz` 并入队，但 `gui_pump_input`（`input.c:295-306`）只处理左键 press/release/drag/hover 与 Esc，从不读 `e.dz`；`dz` 在 GUI 路径的唯一消费是控制台回滚（`render.c:1579-1581`），与块树无关。
- 复现/判据：溢出块上滚动鼠标滚轮，`scroll_y/scroll_x` 不变；CE 应用收不到 `ev_kind=4`（wheel）事件。`decl_post_wheel` 虽组装了 `wheel_dx/wheel_dy/mods`，但生产者不存在。
- 对照规范：§4.12 行 593（M-CAP2-22，断言 `cap2-sb-wheel`）；实现文档 `:19`“内核统一 track+thumb+滚轮+拖 thumb”。
- 建议修法：输入层补滚轮事件 → 命中溢出块调 `sb_wheel` → 归属 owner 调 `decl_post_wheel`。

### A3〔中〕拖 thumb 链路未接线：`sb_mouse` 无生产调用，探针也未覆盖

- 位置：`src/gui/sb.c:130`（`sb_mouse`）；`src/gui/input.c` 无引用。
- 证据：全树检索 `sb_mouse` 仅定义 + 符号表；`on_press/on_drag_move/on_release`（`input.c:288-305`）只走 legacy `on_click/on_drag` 钩子，不识别滚动条轨道/thumb。
- 复现/判据：按住 thumb 拖动，`scroll_y/scroll_x` 不跟随；松开后拖拽状态不复位。
- 建议修法：press 命中轨道/thumb、drag 移动、release 三阶段分发 `sb_mouse(b,x,y,phase)`。

### A4〔中〕`border_sides` 位掩码无绘制消费点，声明单边/双边仍整圈描边

- 位置：镜像 `src/gui/decl.c:476`、`src/gui/desc.c:60,79`；绘制 `src/gui/paint.c:702-712`（`body_border_cap2`）、`paint.c:726-729`（`body_border`）。
- 证据：`body_border_cap2` 先整圆角矩形填边框色，再内缩一圈填背景，四条边始终全画；全 `src/gui` 检索 `border_sides`，除 decl/desc 两处写入外无读取点。
- 复现/判据：`border_sides=bit0(l)` 只应画左边，右/上/下边仍出现边框线。
- 对照规范：§4.12 行 577（M-CAP2-06，consumer `body_border`，断言 `cap2-border-sides`）；实现文档 `:40`。
- 建议修法：描边按 bit0 l / bit1 r / bit2 t / bit3 b 分边填充，掩码 0 维持全边旧行为。

### A5〔中〕`hc_force` 不下传子树：只判定本块，后代不继承 HC 降级

- 位置：`src/gui/paint.c:548-551`（`hc_forced`）、`src/gui/shadows.c:69`、`src/gui/desc.c:80`。
- 证据：
  ```c
  static int hc_forced(block *b) {
      const struct style *tok = gui_active_style();
      return tok->radius_hc_zero || b->hc_force;   /* 无父链 OR 聚合 */
  }
  ```
  全树无“提交/挂载时把父 `hc_force` 镜像给后代”的代码。
- 复现/判据：父块 `hc_force=1`、子块未声明 → 子块阴影/圆角仍按非 HC 主题渲染，与“本子树 HC”口径不符。
- 对照规范：§4.12 行 594、609（“向下作用于本子树”，断言 `cap2-hc-override`）；实现文档 `:41,81`。
- 建议修法：`hc_forced` 沿 `parent` 链 OR 聚合，或 commit/挂载时把父标志向下镜像。

### A6〔中〕`decl_post_resize` / `decl_post_close` 仅自检调用，生产链路无事件生产者

- 位置：`src/gui/decl.c:827`（resize）、`decl.c:841`（close）；唯一调用 `decl.c:1176-1177`（自检块）。
- 复现/判据：布局变化不投递 `DECL_EV_RESIZE(4)`；块关闭不投递 `DECL_EV_CLOSE(5)`（浮层关闭走的是 `decl_post_dismiss`，`decl.c:887`）。ABI 声明了这两类事件，应用侧若等待 resize/close 将永不收到。
- 建议修法：确认 ABI 是否承诺；若承诺，在布局变更/块移除路径补投递；否则从契约文档标注“未生产”。

### A7〔低〕锚点悬空在内置/运行期路径静默回退父框（CE 提交期校验已挡，布局层未挡）

- 位置：`src/gui/popover.c:68-78`（`anchor_rect`）、`src/gui/decl.c:484-495`；CE 校验 `decl.c:299-307`。
- 证据：锚块取不到或不可见 → 回退父框矩形；`decl.c:491-492` 跨 pid/悬空把 `anchor` 置 0。`validate2` 只在 CE 提交时拦截；内置 spec 路径、或运行期锚块被移除后，布局层静默按父框放置。
- 对照规范：§4.12.3 行 605“禁止静默落 0 冒充父框锚定”。
- 建议修法：布局层对 `anchor_id!=0` 解析失败记诊断，回退策略显式化而非静默。

---

## 二、实现了但无消费点 / 只写不读 / 只被探针调用

### B-1〔中〕`anim_ease > 7` 可达越界读 `ease_tab[8][19]`（生产暂不可达，探针路径可达）

- 位置：`src/gui/anim.c:38`（`ease_tab[8][...]`）、`anim.c:86`（仅挡 `ease==0`）、`anim.c:132`（`ease_tab[s->ease][idx]`）。
- 证据：`anim_kick` 放行 `ease=8..255` 建档；`anim_apply` 按下标直接取表。当前因 A1 生产从不 kick，该越界仅探针/未来接线后可达。
- 建议修法：kick 时钳制 `ease <= 7`，或 `validate2` 拒绝 `anim_ease>7`。

### B-2〔低〕`icon >= 8` 静默回退到 0 号菜单图标，规范标称 0..255

- 位置：`src/gui/icons.c:71-72`（`if (ic >= 8) ic = 0;`）；位图仅 `ICON_GLYPH[8][16]`（`icons.c:16`）。
- 判据：`icon=8..255` 不报错、不缺省空白，直接画成 menu 图标；与 `block.h:166`/规范“图标号 0..255”口径不符。
- 建议修法：文档标注实际位图范围 0..7，越界不画或走占位符。

### B-3〔低〕`decl_event_poll` / `poll2` 忽略 `timeout_ms`，非阻塞自旋

- 位置：`src/gui/decl.c:936`、`decl.c:948`（`(void)timeout_ms;`）。
- 判据：超时参数被丢弃，队列空立即返回 0；应用必须自行轮询。属既有旧口径延续，非 cap2 新引入，记录备查。
- 建议修法：若 ABI 承诺超时等待，补调度睡眠；否则文档写明非阻塞。

### B-4〔低〕探针专用辅助符号随镜像发布（说明性，非缺陷）

- 位置：`src/gui/popover.c:35-40`、`pop_probe_*` 入口；`src/gui/decl.c` 探针导出 `decl_ce_probe_*`。
- 判据：仅 `cap2p_*.c` 调用，生产不消费；保留在内核符号表中，属可接受的探针门面，记录备查。

---

## 三、探针过了但判据是空 / 证据不闭环

### C1〔中〕abi 门禁不裁决应用自裁结果：日志 `app=FAIL`、`click=0 input=0` 仍判 4/4 PASS

- 位置：`/home/user/work/artifacts/gui-blocks/probes/gates/cap2_abi.py:162-169`（四项断言）、`:170-171`（“reported, not gated”）、`:155`（解析了 `app_verdict` 但未参与判定）。
- 证据（`gate-cap2-abi-blkcap2-final2.log`）：
  ```
  caps=0x000001ff rc2=0 badrec=-39 click=0 id=0 x=0 y=0 input=0 text= rec1=0 app=FAIL
  PASS caps mask / PASS rec2 commit rc2==0 / PASS illegal rec_size / PASS legacy rec1
  PASS cap2_abi
  ```
- 判据：门禁只验 caps 位、commit 返回码、非法 rec_size、旧 rec1 四项 plumbing；`click/input/text/app_verdict` 只打印不判红。应用自裁 FAIL 被绿灯覆盖。
- 备注：真实鼠标/键盘链路由 deskev 门禁补测（`cap2_deskev.py:216-245`，click/坐标/input/文本四项数值断言，日志 `GUI:ev click id=2 x=8 y=4`、`input len=5`）；但 abi 这条绿灯本身不闭环，实现文档 `:153` 已记为“已知遗留”。
- 建议修法：abi 门禁对 `app_verdict=FAIL` 置红，或把“4 项 PASS”措辞限定为 plumbing 四项，不暗示 E2E 通过。

### C2〔中〕规范点名的三个探针用例在内核分派与门禁中均不存在

- 规范引用：§4.12 行 577（`cap2-border-sides`）、行 593（`cap2-sb-wheel`）、行 594（`cap2-hc-override`）。
- 证据：`cap2_render.py:22-25` CASES、`cap2_interact.py:37-41` CASES，及 `src/gui/cap2p_render.c:630-651`、`cap2p_interact.c:442-455` 分派，均无 `border-sides / sb-wheel / hc-override`；全树 grep 无匹配。
- 判据：A2（滚轮）、A4（边框分边）、A5（HC 子树）对应的语义没有任何取点断言，门禁全绿不覆盖这些路径。
- 建议修法：补三个用例（边框分边像素、滚轮步进与 thumb 一致、HC 子树降级），或从规范断言清单撤下对应 M-CAP2 编号。

### C3〔低〕动画探针覆盖面窄：7 缓动只测 ease=5，6 掩码位只测 bit0(x)；中断起点值未断言

- 位置：`src/gui/cap2p_interact.c:342`（`anim_ms=192 anim_ease=5 anim_mask=1`）、`:388`（ease=1 mask=1）、`:404`（中断仅断言 `b->x == 160`）。
- 判据：
  - ease 1/2/3/4/6/7 曲线互异性无落点采样；mask bit1 y / bit2 w / bit3 h / bit4 alpha / bit5 elevation 无断言。
  - 中断接管“起点 = 当前帧值”（`anim.c:98-100`）未被验证：12 帧后 `elapsed>=ms` 必然落目标，即使接管忽略当前值，末值 160 仍会 PASS。
- 建议修法：补逐曲线帧值表断言、逐掩码位断言；中断用例记录接管瞬间 `sx/sy/...` 与当前帧值比对。

### C4〔低〕`tint_role=2(bg)` 未入探针；图标角色四缺其一

- 位置：`src/gui/cap2p_render.c:296`（`role[3] = {3, 4, 1}`，缺 2）；实现 `icons.c:65`（case 2 = `tok->bg`）。
- 判据：代码四角色齐，但门禁只取点 accent/border/fg，bg 角色着色无证据。
- 建议修法：补 bg 角色取点用例。

### C5〔低〕队列溢出与 dismiss 事件回读无探针

- 位置：CE 探针 `src/gui/cap2p_ce.c:86-181`（button 1 事件、input 2 事件，深 8 队列无满队用例）；浮层探针 `cap2p_interact.c:271-294`（`pop-dismiss` 只看 `alive/count/did`，未回读队列里的 `ev_kind=6 / target_id`）。
- 判据：B1 的丢事件路径无回归保护；dismiss 事件结构内容未经 poll2 校验。
- 建议修法：补 9 事件溢出用例（断言丢新策略/计数）与 dismiss 回读用例。

### C6〔低〕文档/注释结构口径偏差：事件前缀写“20B”，实际 24B

- 位置：`include/ce.h:133`（“前 20B 与 ce_blk_event_t 一致”）、实现文档 `:48`；实际 `ce_blk_event_t`（`ce.h:92-98`）= i16+i16+i32×5 = 24B。
- 判据：代码前缀 24B 字节一致（见 D-5），仅注释/文档数字错。
- 建议修法：注释改 24B。

---

## 四、重点核查清单逐条结论

### 4.1 `anim_*`

| 项 | 结论 | 证据 / 说明 |
|---|---|---|
| `anim_mask` bit0..5 六属性是否都生效 | 可疑 | 代码路径齐：`anim.c:141-152`（x/y/w/h/alpha/elevation）；但生产从不建档（A1），探针只测 bit0（C3）。 |
| `anim_ms==0` 是否零开销 | 通过 | `anim.c:86` `!b->anim_ms` 早退不建档；无槽位时 `anim_tick` 不建表（`anim.c:106-110`）。 |
| `anim_ease 0..7` 八条曲线是否不同 | 可疑 | 0=none；1..7 数学互异（`anim.c:51-58`：linear/in/out/inout/out-back/in-back/step）；仅 ease=5 被采样，且 >7 无钳制（B-1）。 |
| 中断接管是否读当前帧值 | 可疑 | 代码读当前帧：`anim.c:98-100`；探针未断言起点值（C3）。 |
| elevation 动画取档口径 | 低 | `anim.c:146-151` 过半切档（阈值 500）并钳 0..5；注释称“最近档”，实现是阈值切换，语义略有偏差。 |

### 4.2 `corner_n` / `corner_shape`

| 项 | 结论 | 证据 / 说明 |
|---|---|---|
| CE 是否只下发 `corner_shape`、不下发 `corner_n` | 通过 | `ce.h:127-128` rec2 无 `corner_n` 字段；`decl.c:407` 注释、`apply2` 不映射 n。 |
| `corner_n=0` 在 SQUIRCLE 下落到哪个 n | 通过 | `paint.c:323`：n==0 或 50 → SQ5，n10=50。 |
| 越界钳 50 与 `WARN_RADIUS_N_RANGE` 是否有触发路径 | 可疑 | 计数路径在：`paint.c:324-329`，探针 `shape-clamp` 实测 `warn=1`（render 日志）；但 ① 仅内存计数、无串口告警行；② 实现只认 0/45/50，n=10..44/46..49 也会告警（规范有效域 10..50）。 |
| `corner_shape` 在 `decl.c` 是否真到块字段 | 通过 | `decl.c:481` 镜像 `corner_shape`；非 1 一律 ARC（`paint.c:320`）。 |

### 4.3 `ps` per-pid 事件队列

| 项 | 结论 | 证据 / 说明 |
|---|---|---|
| 容量 | 通过 | `decl.c:34` `DECL_EVQ=8`；队列 `decl.c:51`。 |
| 写满行为：丢/覆盖/报错 | 问题 | 静默丢新事件：`decl.c:690-691` 满即 `return`，不覆盖、不报错、不加计数（B1）。 |
| `enqueue2` 返回 void 溢出是否静默 | 问题 | `decl.c:689` `static void enqueue2(...)`，满队无任何回传（B1）。 |
| `poll2` 与旧 24B `decl_event_poll` 是否同源同队列 | 通过 | 同一 `p->evq`：旧 `decl.c:942`、新 `decl.c:953`；注释 `decl.c:51`。 |

### 4.4 scrollbar

| 项 | 结论 | 证据 / 说明 |
|---|---|---|
| 拖 thumb 边界（拖出轨道/最小尺寸/除零/越界） | 通过（代码守卫） | thumb 最小 16、不超轨道：`sb.c:57-58,70-71`；内容高 0 除零守卫：`sb.c:55,60,68,73`；拖拽 frac 钳 0/maxs：`sb.c:174-185`。但生产未接线（A3）。 |
| 滚轮 3 行是否与 thumb 位置一致 | 通过（代码口径） | step=3×行高：`sb.c:102-103`；thumb 位置由同一 `scroll_y/scroll_x` 推导：`sb.c:60,73`。但滚轮未接线（A2）。 |
| `scrollbar=2`(never) | 通过 | `sb.c:45` 显式返回 0；`=1` always、`=0` auto 分支齐。 |
| 跨块拖拽残留 | 低 | `sb.c:126` 静态 `sb_drag_block`；release 仅在同块清状态（`:162`），拖出到别的块再松开，旧块拖拽态残留。 |

### 4.5 `decl_post_dismiss` 24B 兼容路径

| 项 | 结论 | 证据 / 说明 |
|---|---|---|
| `ev_kind=6` 在旧结构落在哪个字节 | 通过 | ev2 布局 `ce.h:134-147`：`target_id` 24..27，`ev_kind` 偏移 28；旧 ev1 仅 24B（`ce.h:92-98`），看不到该字节。 |
| 旧应用是否会被误读成 CLOSE | 通过（设计如此，备注） | dismiss `ev.type=DECL_EV_CLOSE(5)`：`decl.c:893-894`；旧 poll 拷前 24B 读到 type=5，按 CLOSE 处理。旧应用无法区分 dismiss 与普通 close（`decl_post_close` 同为 kind=6，`decl.c:848`）。 |
| 旧 poll 拷前 24B 字段对齐 | 通过 | ev2 前缀 type/code/id/x/y/w/h 与 ev1 同序同型；`decl.c:942` `memcpy` 前 `sizeof(ce_blk_event_t)`。 |

### 4.6 附带项

| 项 | 结论 | 证据 / 说明 |
|---|---|---|
| `alpha=0`（未声明≡255） | 通过 | `paint.c:503` `ca = c->alpha ? c->alpha : 255`，父链相乘。 |
| `alpha=1` 抖动下限 | 通过 | `paint.c:506` 乘积为 0 时下取 1；`alpha_cov_px` 16 档 Bayer：`paint.c:511-519`。 |
| `elevation` 0..5、焦点 +1 上限 5 | 可疑 | 焦点 +1 有上限：`shadows.c:66`；声明值 >5 无钳制，越界读（B2）。 |
| `wrap_lines=255`(auto) 与 `ellipsis` 组合 | 通过 | `textlayout.c:145` auto→6 行；省略号分支 `:161,174`；探针 `wrapauto lines=6 ell=1`（text 日志）。 |
| `BLK_ICON` 的 `tint` 四角色 | 可疑 | 代码齐：`icons.c:63-68`（1 fg/2 bg/3 accent/4 border，缺省 fg）；bg 角色未探针（C4）。 |
| `pop_max_h=0` 语义 | 通过 | `popover.c:105` `>0` 才限高，0 不限。 |
| `line_h=0` 走令牌默认 | 通过 | 文本包行高分支（`textlayout.c:292` 一带）；探针覆盖 wrap 系列。 |
| `font_size` 越界 | 通过 | `textlayout.c:35-41` `slot_idx` 缺省回 0 档，无槽位越界。 |

---

## 五、审计总表

| # | 审计项 | 类别 | 严重度 | 结论 |
|---|---|---|---|---|
| 1 | `anim_mask` 六属性代码路径 | 一 | — | 通过 |
| 2 | `anim_ms==0` 零开销 | 一 | — | 通过 |
| 3 | `anim_ease` 0..7 曲线互异 | 三 | 低 | 可疑 |
| 4 | 中断接管读当前帧值 | 三 | 低 | 可疑 |
| 5 | `anim_kick` 生产建档触发 | 一 | 高 | 问题 |
| 6 | `anim_ease>7` 越界表 | 二 | 中 | 问题 |
| 7 | CE 不下发 `corner_n` | 一 | — | 通过 |
| 8 | `corner_n=0`→SQ5 | 一 | — | 通过 |
| 9 | 越界钳 50 + WARN 路径 | 三 | 低 | 可疑 |
| 10 | 事件队列容量 8 | 一 | — | 通过 |
| 11 | 满队静默丢新事件 | 一/阻塞 | 高 | 问题 |
| 12 | `enqueue2` void 无溢出回传 | 二 | 中 | 问题 |
| 13 | poll2/旧 poll 同源队列 | 一 | — | 通过 |
| 14 | scrollbar thumb 边界守卫 | 一 | — | 通过 |
| 15 | 滚轮 3 行与 thumb 一致（代码） | 一 | — | 通过 |
| 16 | `sb_wheel` 生产接线 | 一 | 高 | 问题 |
| 17 | `decl_post_wheel` 生产者 | 二 | 中 | 问题 |
| 18 | `sb_mouse` 生产接线 | 一 | 中 | 问题 |
| 19 | dismiss 24B 字节落点 | 一 | — | 通过 |
| 20 | 旧应用按 CLOSE 读 dismiss | 一 | — | 通过 |
| 21 | 旧 poll 24B 前缀对齐 | 一 | — | 通过 |
| 22 | `alpha=0`≡255 / `alpha=1` 下限 | 一 | — | 通过 |
| 23 | elevation 焦点 +1 上限 5 | 一 | — | 通过 |
| 24 | `elevation>5` 无钳制 OOB | 一/阻塞 | 高 | 问题 |
| 25 | `wrap=255` + ellipsis 组合 | 一 | — | 通过 |
| 26 | tint 四角色代码 | 一 | — | 通过 |
| 27 | tint bg 角色探针覆盖 | 三 | 低 | 可疑 |
| 28 | `pop_max_h=0` 不限 | 一 | — | 通过 |
| 29 | `corner_shape` CE→块字段 | 一 | — | 通过 |
| 30 | `border_sides` 绘制消费 | 一 | 中 | 问题 |
| 31 | `hc_force` 子树继承 | 一 | 中 | 问题 |
| 32 | 锚点悬空内置路径静默回退 | 一 | 低 | 问题 |
| 33 | resize/close 事件生产路径 | 二 | 低 | 问题 |
| 34 | `icon>=8` 回退 0 | 二 | 低 | 问题 |
| 35 | poll 忽略 `timeout_ms` | 二 | 低 | 问题 |
| 36 | abi 门禁忽略 `app=FAIL` | 三 | 中 | 问题 |
| 37 | 缺失探针：border-sides/sb-wheel/hc-override | 三 | 中 | 问题 |
| 38 | 动画探针覆盖（曲线/掩码/起点） | 三 | 低 | 可疑 |
| 39 | 队列溢出/dismiss 回读探针 | 三 | 低 | 可疑 |
| 40 | 事件前缀 20B/24B 文档口径 | 三 | 低 | 问题 |

**汇总**：审计 40 项 —— 通过 17，可疑 6，问题 17。其中阻塞项 2 条（#11 满队静默丢事件、#24 elevation>5 越界读）；高严重度 4 条（#5 动画不建档、#16 滚轮未接线、#11、#24）。

### 最硬的两个例子

- **阻塞 #24**：`src/gui/shadows.c:76` `uint32_t str = tok->shadow[el];`，`el` 来自 `src/gui/paint.c:791` `sh_paint(b, b->elevation)`，入参未钳制；`tokens.h:36` `shadow[6]`，`shadows.c:21` `SH_PAD[6]`。`validate2`（`decl.c:237-310`）不查 elevation 值域，`decl.c:460` 原样镜像 → `elevation=6..255` 越界读。
- **阻塞 #11**：`src/gui/decl.c:690-691`
  ```c
  int next = (p->evt + 1) % DECL_EVQ;
  if (next == p->evh) return;
  ```
  队列深 8（`decl.c:34`），满队直接丢新事件；`enqueue2` 返回 void（`decl.c:689`），`stat_events` 不增（`decl.c:695` 仅入队才计），应用与 poll 侧无丢失感知。
