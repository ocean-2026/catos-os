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
- 其余组件（记事本 / 画布 / 图片 / 浏览器 / 设置）未接本套多实例聚焦路由，沿用基线行为（见「被砍项」）。

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

1. **其它组件点击聚焦未接**（砍项顺序第 3 项）：记事本、画布、图片查看器、浏览器、设置仍走基线单例 / 既有 hook 路径，未纳入本次多实例聚焦路由。
2. **终端实例池保留静态 .bss**：基线 `terms[4]` 本就在 .bss（链接门禁既有 WARN，非 fatal）；本轮仅为每实例补 `num`/`title_blk` 两个字段（+0x20），未把终端池改堆。FM 实例数组按要求全部走堆。
3. **窗口叠放沿用基线错位**：终端/FM 多窗错位幅度继承基线（终端 20×16、FM 20×16），后窗身体被前窗部分遮挡；两实例目录/选中/缓冲隔离以串口打点为准，像素截图仅作同屏存在性佐证。
4. **`pwd` 非 shell 内置**：B 窗口目录由探针经 `fs_pwd` 口径读 `gui_term_cwd(1)` 上报（`cwd1=/`），未在 shell 里跑 `pwd` 命令；B 窗口内可见命令为 `ls`。
5. **截图 VGA 后端**：本机 `-vga vmware` 的 HMP screendump 取到陈旧前台缓冲（窗口已建块但截图整屏底色），取证脚本固定 `-vga std`；与产品代码无关。
6. FM 第 3 实例（`FM_MAX=3`）代码路径就位，本轮取证只开 2 实例；第 3 实例复用同一 `fm_inst_t` 分配/聚焦/关闭路径。

## 5. 红线核对

- CE ABI：未改 `ce_abi.h`；`gui.h` 仅纯追加内部探针口径声明。
- 未引 GPL/AGPL；未改版本号；ISO/bin 未入库；未改用户六份正文文档。
- 命名：未使用 `window` / `win_id` / `wm_` 前缀。
- .bss：FM 大数组（names/raw/labels）已转堆；终端池为基线既有静态结构，未新增大数组。
- 标脏：关闭路径只标被关块 + 旧矩形，未退回整屏 `gui_mark_dirty(root)`。
