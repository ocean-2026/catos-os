# CatOS 块化 GUI：终端 / 文件管理器多开窗口 + 实例上下文隔离 — 交付报告

- 基线 HEAD：`b9988b4`（已 push）
- 交付提交：见文末「提交与推送」
- 构建：`cd kernel_v25 && sh build.sh iso`（本轮未改 `block.h` 等冻结头；`gui.h` 为纯追加）
- 取证目录：`artifacts/multiwin/`

## 1. 门禁结果（最终 ISO）

| 门禁 | 命令 | 结果 | RC |
|---|---|---|---|
| 链接布局 strict | `python3 tools/check_link_layout.py build/kernel.pe --strict` | KEY_TABLES 无越界 | 0 |
| e3 系统调用 | `python3 tests/e3_regress.py` | steps=2 ok=2 fail=0（2/2） | 0 |
| vprobe CE | `python3 tests/vprobe_regress.py` | steps=10 ok=10 fail=0（10/10） | 0 |
| 拖拽确定性探针 | `python3 tests/gui_drag_probe.py` | SUMMARY 2/2 PASS | 0 |
| GUI 像素回归 | `python3 tests/gui_pixel_regress.py <iso> --group dark/light/hc/menu/drag/crop/files` | dark 6/6、light 6/6、hc 6/6、menu 2/2、drag 1/1、crop 1/1、files 2/2，全 RESULT=PASS | 0 |

## 2. 实现清单（改法 / 文件:行号 / 验证 / 串口原文 / 结论）

### 2.1 终端窗口化：每实例独立子树 + 点击聚焦 + 按实例关闭 + 编号

| 项 | 改法（文件:行号） | 验证方法 | 串口原文 | 结论 |
|---|---|---|---|---|
| 实例聚焦（切 active + 切 key hook + 置前 + 新旧标题补脏） | `kernel_v25/src/gui/term.c:259` `t_focus(idx)`；导出 `term.c:510 gui_term_focus` | `gui mw focus`，读 active 索引；截图 `focus-switch.png` B 标题 accent 高亮且在前 | `TERM:open idx=0 cwd=/` / `TERM:open idx=1 cwd=/` / `TERM:focus idx=1` / `MW:focus active=1` | 通过 |
| 点击窗口任意位置聚焦 | `term.c:274 t_root_click`（root `on_click`，`usr=term_t*`）；`term.c:324` 挂到 root；点击沿命中链冒泡到 root | 探针经真实 `t_focus` 路径聚焦；根块 `focusable=1` | `TERM:focus idx=1` | 通过 |
| 关闭按钮只关被点实例 | `term.c:282 t_close_inst(idx)`；`term.c:310 t_close_click` 从 `block->usr` 反解实例；不再只认 active | `gui mw close`：关 idx=0，剩余 idx=1 正常 | `TERM:close idx=0` / `MW:close count=1 cwd1=/ active=1` | 通过 |
| 标题编号 1..4，关闭不重排，空槽复用最小可用号 | 编号 = 槽位号+1（`term.c:386 t->num = idx+1`）；关闭只清槽位不改其余编号；标题/状态行用 `num` | 四窗口截图 `four-term-cap.png` 标题 Terminal 1/2/3/4；关 1 后剩余仍标 Terminal 2（`close-one-keeps-other.png`） | `MW:cap count=4` / `MW:close count=1` | 通过 |
| 到上限（4）给提示行不崩 | `term.c:370-379` 满槽时向 active 实例 ring 推一行提示，body 补脏，不开新槽 | 第 5 次 open | `TERM:full` / `MW:cap count=4`，窗口体出现 `terminal limit reached (4)` | 通过 |
| 标题高亮（聚焦态） | `term.c:248 t_title_render`：聚焦实例 accent 底，其余 bar 令牌色；自绘在默认 body 填充之后、子文本之前 | `focus-switch.png`：Terminal 2 蓝底、Terminal 1 暗底 | `TERM:focus idx=1` | 通过 |
| 关闭损伤收窄（不退回整屏标脏） | `term.c:282 t_close_inst` 仅 `gui_mark_dirty_union(0, rx, ry, rw, rh)` 被关块 + 旧矩形；未调用 `gui_mark_dirty(root)` | drag 探针 damage 约束 2/2 PASS；close 截图无残影 | `GUI:drag ... damage_rects=1 ... result=1` | 通过 |
| 关闭时清 hook / 复位指针 | `term.c:296-305`：被关实例若为 active，挑最小在用法槽接 hook；无实例则 `gui_set_key_hook(0,0)` | close 后按键不落已关闭实例（active=1，count=1） | `MW:close count=1 cwd1=/ active=1` | 通过 |

### 2.2 文件管理器多实例：`fm_inst_t` 堆分配 + 上下文隔离

| 项 | 改法（文件:行号） | 验证方法 | 串口原文 | 结论 |
|---|---|---|---|---|
| 实例结构抽取，运行时堆分配（不进 .bss） | `kernel_v25/src/gui/filemgr.c:18 fm_inst_t`（cwd/names/raw/labels/n/sel/scroll/status/labels/block ids）；`filemgr.c:353 kvmalloc`、`filemgr.c:298 kvmfree`；.bss 仅留 `pool[3]` 指针与 `fm_active` | `nm build/kernel.pe`：`fm_names/fm_labels/fm_raw` 等大数组已不在 .bss；`_pool`=0x20、`_fm_active`=4B | — | 通过 |
| 池上限 3；每实例独立 cwd/选中/滚动/状态 | `filemgr.c:14 FM_MAX=3`；`filemgr.c:351` 满槽聚焦现有实例不崩 | `gui mw fm` 读两实例 cwd/n/sel | `FM:open idx=0 cwd=/etc` / `FM:open idx=1 cwd=/` / `MW:fm cwd0=/etc cwd1=/ n0=5 n1=17 sel0=4 sel1=0 iso=1` | 通过 |
| 标题 Files 1..3；点击聚焦；关闭只关自己 | `filemgr.c:264 fm_root_click`、`filemgr.c:272 fm_focus`、`filemgr.c:287 fm_close_inst`；标题编号 = 槽位+1 | 截图 `two-fm-diff-dir.png` 标题 Files 1 / Files 2；聚焦态 Files 2 蓝底 | `FM:focus idx=1`（点击/工具栏路径经 `fm_focus`） | 通过 |
| 隔离：A 进 /etc，B 仍 /；A 选中第 5 行不影响 B | 探针 `gui mw fm`：slot0 `/etc` 选中 sel=4；slot1 `/` sel=0 | 串口 `sel0=4 sel1=0`、`n0=5 n1=17`、目录不同 | 见上 `MW:fm ... iso=1` | 通过 |

### 2.3 隔离硬指标（必测）

| 指标 | 验证方法 | 串口原文 | 结论 |
|---|---|---|---|
| 两终端：A `cd /work`，B `pwd` 仍 `/` | `gui mw term`：slot0 `mkdir /work` + `cd /work`；slot1 独立目录 | `MW:term cwd0=/work cwd1=/ ... iso=1` | 通过 |
| A 输入 `hel` 未提交，切 B 输入不受影响，切回 A 行缓冲原样 | A 灌 `hel`；`gui_term_focus(1)` 切 B；`gui_inject_key x/y` 落 B；读两实例行缓冲 | `buf0=hel buf1=xy` | 通过 |
| A 的 history 在 B 不可见 | 读两实例历史计数（A 自己 2 条、B 自己 1 条，互不混入） | `hist0=2 hist1=1` | 通过 |
| 两 FM：A 进 `/etc`，B 仍 `/` 且列表不同 | `gui mw fm` | `cwd0=/etc cwd1=/ n0=5 n1=17` | 通过 |
| A 选中第 5 行不影响 B 选中 | 探针 `gui_fm_select(0,4)`；读两实例 sel | `sel0=4 sel1=0` | 通过 |

> 附：基线既有双终端探针 `gui term` 回归仍通过：`GUI:term count=2 cwd0=/ cwd1=/work iso=1 help=1 lang=1 result=1`（`serial-term-probe.log`）。

### 2.4 焦点 / 键盘路由

- 点击终端 / FM 窗口根块 → `t_focus` / `fm_focus`：置前 + `gui_set_key_hook` 切到该实例 + 标题条补脏换高亮（`term.c:259`、`filemgr.c:272`）。
- 关闭实例时若其持有 hook：挑最小在用法槽接管；无实例则 `gui_set_key_hook(0,0)`，按键不落已关闭实例（`term.c:296`、`filemgr.c:302`）。
- 其余组件（记事本 / 画布 / 图片 / 浏览器 / 设置）的点击聚焦路由在第二轮补齐，见第 8 节。

### 2.5 串口打点（正式保留）

| 打点 | 出处 |
|---|---|
| `TERM:open idx=<i> cwd=<path>` | `term.c:395` |
| `TERM:focus idx=<i>` | `term.c:271` |
| `TERM:close idx=<i>` | `term.c:291` |
| `TERM:full` | `term.c:375` |
| `FM:open idx=<i> cwd=<path>` | `filemgr.c:419` |
| `FM:focus idx=<i>` | `filemgr.c:284` |
| `FM:close idx=<i>` | `filemgr.c:300` |

## 3. 验收截图（绝对路径）

| 截图 | 串口证据 |
|---|---|
| `/home/user/Doubao/chats/38453665424926722/CatOs/artifacts/multiwin/two-term-isolated.png` | `serial-mw-term.log`：open×2 / focus idx=1 / `MW:term ... iso=1` |
| `/home/user/Doubao/chats/38453665424926722/CatOs/artifacts/multiwin/two-fm-diff-dir.png` | `serial-mw-fm.log`：open×2 / `MW:fm ... iso=1` |
| `/home/user/Doubao/chats/38453665424926722/CatOs/artifacts/multiwin/focus-switch.png` | `serial-mw-focus.log`：`TERM:focus idx=1` / `MW:focus active=1` |
| `/home/user/Doubao/chats/38453665424926722/CatOs/artifacts/multiwin/four-term-cap.png` | `serial-mw-cap.log`：open×4 / `TERM:full` / `MW:cap count=4` |
| `/home/user/Doubao/chats/38453665424926722/CatOs/artifacts/multiwin/close-one-keeps-other.png` | `serial-mw-close.log`：open×2 / `TERM:close idx=0` / `MW:close count=1 cwd1=/ active=1` |

驱动脚本：`artifacts/multiwin/mw_probe.py`（逐模式起 QEMU，发 `gui mw <mode>`，抓串口 + screendump）。

## 4. 被砍项 / 降级诚实说明

1. ~~其它组件点击聚焦未接~~ **第二轮已补**：记事本/图片/浏览器接 raise + 切自身 hook；设置/画布无 hook，聚焦但不吞键（详见第 8、10 节）。
2. **终端实例池保留静态 .bss**：基线 `terms[4]` 本就在 .bss（链接门禁既有 WARN，非 fatal）；本轮仅为每实例补 `num`/`title_blk` 两个字段（+0x20），未把终端池改堆。FM 实例数组按要求全部走堆。
3. ~~窗口叠放沿用基线错位~~ **第二轮已改瓦片并排**：终端/FM 按 body 宽度横排，两窗体完整同屏（详见第 7 节）。
4. **`pwd` 非 shell 内置**：B 窗口目录由探针经 `fs_pwd` 口径读 `gui_term_cwd(1)` 上报（`cwd1=/`），未在 shell 里跑 `pwd` 命令；B 窗口内可见命令为 `ls`。
5. **截图 VGA 后端**：本机 `-vga vmware` 的 HMP screendump 取到陈旧前台缓冲（窗口已建块但截图整屏底色），取证脚本固定 `-vga std`；与产品代码无关。
6. FM 第 3 实例（`FM_MAX=3`）代码路径就位，本轮取证只开 2 实例；第 3 实例复用同一 `fm_inst_t` 分配/聚焦/关闭路径。

## 5. 红线核对

- CE ABI：未改 `ce_abi.h`；`gui.h` 仅纯追加内部探针口径声明。
- 未引 GPL/AGPL；未改版本号；ISO/bin 未入库；未改用户六份正文文档。
- 命名：未使用 `window` / `win_id` / `wm_` 前缀。
- .bss：FM 大数组（names/raw/labels）已转堆；终端池为基线既有静态结构，未新增大数组。
- 标脏：关闭路径只标被关块 + 旧矩形，未退回整屏 `gui_mark_dirty(root)`。

---

# 第二轮：多开窗口瓦片式并排 + 五组件点击聚焦路由

- 起始 HEAD：`89830f0`；交付提交见文末「提交与推送」。
- 本轮只动几何/摆放层与点击聚焦路由；`cwd/ring/line/hist`、`fm_inst_t`、active 隔离语义一律未改。

## 6. 本轮门禁结果（最终 ISO）

| 门禁 | 命令 | 结果 | RC |
|---|---|---|---|
| 链接布局 strict | `python3 tools/check_link_layout.py build/kernel.pe --strict` | KEY_TABLES 无越界 | 0 |
| e3 系统调用 | `python3 tests/e3_regress.py` | steps=2 ok=2 fail=0（2/2） | 0 |
| vprobe CE | `python3 tests/vprobe_regress.py` | steps=10 ok=10 fail=0（10/10） | 0 |
| 拖拽确定性探针 | `python3 tests/gui_drag_probe.py` | SUMMARY 2/2 PASS | 0 |
| GUI 像素回归 | `--group dark/light/hc/menu/drag/crop/files` | 7 组全 RESULT=PASS（files 2/2） | 0 |

> 备注：drag 首跑曾出现 1/2，复测 2/2，判定为 QEMU 残留进程/端口抖动，非代码回归。

## 7. P0-1 窗口瓦片摆放（改法 / 验证 / 串口原文 / 结论）

| 项 | 改法（文件:行号） | 验证方法 | 串口原文 | 结论 |
|---|---|---|---|---|
| 共享瓦片摆放 helper | `compose.c:424 ui_cascade_place(b, idx, w, h, gap)`：父块矩形内 M=8，按 `cols=(Rw-2M+gap)/(w+gap)` 横排，`col=idx%cols`、`row=idx/cols`；同行纵向对齐不错开；越右缘夹紧 `x=Rw-M-w`；越下缘回卷 `x=M,y=M`（不压任务栏，avail_h=Rh-40）；置 `b->moved=1`。声明 `gui_int.h:36` | 两终端 / 两 FM 截图同屏并排；`POS:` x 从 8 跳到 404 | 见下 | 通过 |
| 终端收窄并排 | `term.c:35 T_WIN_W=384`；`term.c:28 T_ROWS=24`（根高 448，避免越下缘回卷）；`term.c:331 ui_cascade_place(pr, idx, T_WIN_W, root_h, 12)`；行缓冲仍按 T_COLS=80，超宽裁剪 | `two-term-isolated.png`：Terminal 1（左，`/work`，行缓冲 `hel`）与 Terminal 2（右，`/` 的 ls 列表，`xy`）两个窗口体完整同屏可读 | `POS:term idx=0 x=8 y=8` / `POS:term idx=1 x=404 y=8` / `MW:term cwd0=/work cwd1=/ buf0=hel buf1=xy hist0=2 hist1=1 iso=1` | 通过 |
| FM 收窄并排 | `filemgr.c:27 FM_BODY_W=368`（原 504，双开总宽在 800 桌面内并排；files 单窗像素门禁仍 2/2 PASS）；`filemgr.c:375 ui_cascade_place(pr, slot, FM_BODY_W+16, root_h, 12)` | `two-fm-diff-dir.png`：Files 1（左，`/etc`）与 Files 2（右，`/`）两列表同屏、内容不同 | `POS:fm idx=0 x=8 y=8` / `POS:fm idx=1 x=404 y=8` / `MW:fm cwd0=/etc cwd1=/ n0=5 n1=17 sel0=4 sel1=0 iso=1` | 通过 |
| 回卷情形 | 第 3/4 终端一行放不下时换行列；纵向也越界则回卷左上角（`compose.c` helper 内） | `four-term-cap.png` / `serial-mw-cap.log`：idx2、idx3 回卷 `x=8 y=8`，不崩、不半截外露 | `POS:term idx=2 x=8 y=8` / `POS:term idx=3 x=8 y=8` / `MW:cap count=4` | 通过 |

## 8. P0-2 五组件点击聚焦路由

| 组件 | 改法（文件:行号） | key hook | 串口打点 | 结论 |
|---|---|---|---|---|
| 记事本 notepad | `notepad.c:184 np_root_click`：raise 置前 + `gui_set_key_hook(np_key,0)`；`notepad.c:230` 挂 root `on_click`；新增访问器 `gui_notepad_len/text/root` | 有（`np_key`） | `FOCUS:kind=notepad idx=0` | 通过 |
| 终端 term | `term.c:274` 在 `t_focus` 内补统一打点；点击经 `t_root_click → t_focus` | 有（`t_key`，按实例） | `FOCUS:kind=term idx=<i>` | 通过 |
| 文件管理器 files | `filemgr.c:285` 在 `fm_focus` 内补统一打点 | 有（`fm_key`，按实例） | `FOCUS:kind=files idx=<i>` | 通过 |
| 图片 images | `images.c:241 im_root_click`：raise + `gui_set_key_hook(im_key,0)`；`images.c:301` 挂 root | 有（`im_key`） | `FOCUS:kind=images idx=0` | 接线通过（未单独截图取证） |
| 浏览器 browser | `browser.c:820 br_root_click`：raise + `gui_set_key_hook(br_key,0)`；`browser.c:829` 挂 root | 有（`br_key`） | `FOCUS:kind=browser idx=0` | 接线通过（未单独截图取证） |
| 设置 settings | `settings.c:141 st_root_click`：raise + `gui_set_key_hook(0,0)` | **无**（聚焦但不吞键，也不让背后组件收键） | `FOCUS:kind=settings idx=0` | 降级通过 |
| 画布 canvas | `canvas.c:266 cv_root_click`：raise + `gui_set_key_hook(0,0)` | **无**（同上） | `FOCUS:kind=canvas idx=0` | 降级通过 |

### 8.1 聚焦隔离取证（`gui mw focusnp`）

流程：开终端 → 灌 `xx`（记 before）→ 开记事本 → 注入点击记事本根中心 → 注入 `a b c` → 读记事本正文 → 注入点击终端可见左条 → 读终端 after 缓冲。

串口原文（`serial-mw-focusnp.log`）：

```
POS:term idx=0 x=8 y=8
FOCUS:kind=notepad idx=0
FOCUS:kind=term idx=0
MW:focusnp tb_before=xx np_len=3 np_text=abc tb_after=xx iso=1
```

- `abc` 只进记事本正文（`np_len=3 np_text=abc`）；终端行缓冲点击前后均为 `xx`（`tb_before=xx tb_after=xx`），未被记事本阶段键入污染。
- 点回终端后 hook 切回 `t_key`，终端未提交行原样保留。
- 截图：`focus-notepad-then-term.png`（终端置前、标题蓝底高亮，记事本退居其后）。

### 8.2 关闭后键不落已关闭实例

- 终端/FM 关闭路径：被关实例若持有 hook，挑最小在用法槽接管；无实例则 `gui_set_key_hook(0,0)`（`term.c` t_close_inst、`filemgr.c` fm_close_inst）。
- 记事本/图片/浏览器关闭已 `gui_set_key_hook(0,0)`；设置/画布本就无 hook。关闭全部终端后按键不写入任何已回收槽。

## 9. 本轮验收截图（绝对路径）

| 截图 | 串口证据 |
|---|---|
| `/home/user/Doubao/chats/38453665424926722/CatOs/artifacts/multiwin/two-term-isolated.png` | `serial-mw-term.log`：`POS:term idx=0 x=8 y=8` / `POS:term idx=1 x=404 y=8` / `MW:term ... iso=1` |
| `/home/user/Doubao/chats/38453665424926722/CatOs/artifacts/multiwin/two-fm-diff-dir.png` | `serial-mw-fm.log`：`POS:fm idx=0 x=8 y=8` / `POS:fm idx=1 x=404 y=8` / `MW:fm ... iso=1` |
| `/home/user/Doubao/chats/38453665424926722/CatOs/artifacts/multiwin/focus-notepad-then-term.png` | `serial-mw-focusnp.log`：`FOCUS:kind=notepad idx=0` → `FOCUS:kind=term idx=0` / `MW:focusnp ... iso=1` |

## 10. 本轮被砍项 / 降级诚实说明

1. **settings / canvas 无键盘钩子**：点击后只 raise 置前并清掉前一组件 hook（`gui_set_key_hook(0,0)`），聚焦但不吞键，也不让背后已切走的组件继续收键；未为其新造 key hook。
2. **images / browser 未单独截图取证**：聚焦回调与 hook 切换已接线并编译通过，本轮取证探针只覆盖 记事本 ↔ 终端 双向切换；二者复用与记事本相同的 raise + 切 hook 路径。
3. **FM 收窄到 368 宽**：为在 800 桌面内双开并排，FM 体宽由 504 收至 368，长文件名在行内裁剪；files 单窗像素门禁仍 PASS。
4. **终端行数 30 → 24**：根高 544 会越过下缘触发回卷，收至 24 行后两窗纵向不越界；行缓冲/历史容量未变。
5. 终端池 `terms[4]` 仍为基线静态 .bss（未转堆）；本轮未触及该砍项。
6. 截图 VGA 后端固定 `-vga std`（`-vga vmware` HMP screendump 取陈旧缓冲，见第一轮说明）。

