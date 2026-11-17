# TUI 学习储备笔记（2026-11-17）

> 目的：对比 ncurses / Ratatui(tui-rs) / notcurses / DOS EDIT 的组件模型，落到 CatOS `gfx/console` 层可借鉴的设计要点，为后续 TTF/OTF 轮廓字体渲染与 TUI 应用铺路。
> 基线：HEAD `6cf1ff6`（v2.45）。本文为调研储备，不含代码改动。

## 1. CatOS 现有控制台 / 图形层盘点（源码已核实）

**前端（`src/render.c` + `include/render.h`，模式无关契约）**

- 逻辑单元格网格：`cells[CON_MAX_ROWS][CON_MAX_COLS]`；`tmp_cells` 临时缓冲；v2.25 脏追踪 `painted[]`（只提交实际画出的格，`CON_INFO_CELLS_DRAWN/SKIPPED` 可查）。
- 整屏备份：`ui_backup[]`；瞬态状态栏 `saved_row[]` / `con_status_bar()` / `con_status_bar_at()` 保存并逐行恢复原文。
- scrollback：`sb[CON_SB_ROWS=96]` 环形存档 + `sb_live[]` 全量副本；`con_scrollback(delta)` 只画存档窗、不改活网格。
- 定点写 API：`con_putc_at()` / `con_write_at()` / `con_write_at_utf8()`（UTF-8 感知，汉字占两列）；光标 `con_get/set_cursor()`；样式 `con_set_style/fg/attrs`，CLUT 索引或 `0x00RRGGBB`；256 项调色板可写 `con_set_palette()`。
- 尺寸/模式内省：`con_cols()` / `con_rows()` / `con_info(what)` / `con_backend_name()`。

**后端（`src/be_*.c`，全部画文本格）**

- `be_text.c`：VGA text 03h，80×25，16 色，CJK 走动态 8×16 字体槽。
- `be_gfx.c`：Mode 13h，40×12，8bpp 索引。
- `be_vbe.c`：VBE LFB 800×600×32，100×37 真彩。
- `be_vbe24.c`：VBE LFB 800×600×24，100×37；另导出内核侧 `gfx24_*` 绘图 API（`gfx24.h`：`clear/pixel/rect/blit`），帧缓冲 supervisor-only，用户程序拿不到地址。

**键盘抽象（`src/keyboard.c` + `include/keyboard.h`）**

- 阻塞 `kb_getkey()` / 非阻塞 `kb_peek()`；CE ABI slot 64 `pollkey()`。
- 功能键枚举 >0xFF：`KB_UP/DOWN/LEFT/RIGHT/HOME/END/DEL/INS/PGUP/PGDN/F1..F12`；Ctrl+方向有独立码，修饰状态不泄漏给调用方。
- 缺口：无修饰键（Ctrl/Alt/Shift）状态随事件上报；无事件类型/结构；无鼠标、无 Resize 事件。

**字体（`src/render_font.c`）**

- `font8x8_basic`：ASCII 0x20–0x7E，8×8 点阵；`hz_font`：GB2312 16×16 汉字，GNU Unifont 派生（`font_hz.inc`，`tools/gen_hz_font.py` 生成）；按码点二分查表。
- 纯位图：无轮廓、无缩放、无抗锯齿；字符集受位图表覆盖限制。

**现有 TUI 雏形**

- `extapp/hexedit_port.c`：自定义键码表（含 `KB_CTRL(c)` 组合键），十六进制查看器式布局。
- shell 行编辑器：nano 风格，光标位置以 `con_get_cursor()` 为锚做原地重绘。
- `cmd_fortune`：ASCII `-`/`|` 引文框（按显示宽度对齐 CJK）；`extapp/spinner.c`：后台动画轮询范例；拼音 IME 状态栏。

## 2. 对标组件模型

### 2.1 ncurses（经典 C TUI 库）

- **双层屏 diff**：`stdscr`（虚拟逻辑屏）与 `curscr`（终端实屏）分离；`refresh()` 比对两屏，只把变化的字符与属性写到终端，减少输出量。
- **窗口与面板**：`WINDOW` 独立缓冲 + 起点坐标 + 裁剪矩形；`derwin` 相对父窗口嵌套；panels 库在窗口之上再加 z 序栈，支持遮挡、重排、置顶。
- **按键解析**：`keypad()` 打开终端功能键转义序列解析，方向键/功能键映射为 `KEY_*` 常量；`raw()`/`cbreak()` 关闭行缓冲与回显。
- **备用屏幕**：经 terminfo `smcup`/`rmcup` 能力进场切备用屏、退场恢复原屏，TUI 内容不污染 scrollback。
- **边框与样式**：`box()`/`border()` 用 ACS 制表符画线；属性 `A_BOLD/A_REVERSE/A_UNDERLINE`；颜色对（pair number → fg/bg）。
- **鼠标**：`mousemask()` 订阅事件，`getmouse()` 取坐标与按键。
- **保留模式**：控件状态驻留窗口缓冲，更新时做局部脏矩形重绘。

### 2.2 Ratatui / tui-rs（即时模式 Rust TUI）

- **即时模式 + 中间缓冲**：每帧应用把全部 widget 重绘进中间 `Frame` 缓冲；库做 diff 后只向后端写变化；不保留控件树状态（与 ncurses 保留模式相对）。
- **约束式布局**：`Layout` 水平/垂直分栏，支持比例、固定长度、最小约束；嵌套布局递归产出每个 widget 的 `Area` 矩形。
- **Widget 抽象**：`render(frame, area)` 接口；内置 List/Table/Paragraph/Block/Gauge/Sparkline/Chart 等；`Block` 带标题与边框。
- **事件外置**：库不内含事件处理，由后端（crossterm/termion）`event::read()` 取 Key/Mouse/Resize 事件，应用自管状态机。
- **进出场**：`init()` 进 alternate screen + raw mode，panic hook 保证 `restore()` 执行；`restore()` 回退原终端状态。
- **文本模型**：`Text` = 多行 `Line`；`Line` = 多 `Span`；`Span` = 字符串 + `Style`（fg/bg/修饰）。
- **Resize**：事件驱动重布局，界面自适应终端尺寸。

### 2.3 notcurses（平面叠加现代 TUI）

- **ncplane**：可任意大小、位置、z 序的平面；标准平面铺满物理屏；多平面在每个单元格上按 z 序相交，自上而下取第一个非透明单元。
- **行级 damage map**：每平面每行一个布尔脏标记；渲染分两阶段——先生成场景（确定每物理格的字形簇、颜色、样式），再 blit 合成 UTF-8 序列与转义码输出。
- **颜色与字形**：原生 24-bit RGB，索引色终端量化；扩展字形簇（EGC）处理组合字符；支持透明度与 alpha 混合。

### 2.4 DOS EDIT / QBasic IDE（字符界面经典范式）

- **结构**：顶部菜单栏（File/Edit/View/Help）+ 下拉弹出层；中央编辑区；底部快捷键提示行 / 状态行（行列、编码）。
- **modeless**：无 vim 式模式切换；菜单快捷键 + 鼠标（有驱动时）；双 pane 水平分屏；2.0 支持最多 9 个文档 MDI。
- **字符画框**：菜单条、边框、滚动条全部用制表符/字符排布；无独立 widget 工具箱，界面是一次性字符布局。
- **约束**：颜色方案可配；受常规内存限制，编辑缓冲小。

## 3. 可借鉴的 CatOS 设计要点

### 3.1 双缓冲与面板/窗口抽象

- 现状已有 `cells` 逻辑格 + `painted` 脏追踪 + `tmp_cells`，定点写 API 齐备。
- **借鉴**：在前端之上加 panel/window 层——每面板独立格缓冲（或矩形区域 + 坐标 + z 序 + 裁剪标志）；合成阶段按 z 序把面板格 blit 进 `cells`，透明格不覆盖；再走现有 painted diff 提交后端。
- 落地次序：第一步只做全屏单面板 TUI；多面板栈、遮挡裁剪后置。现有 `ui_backup` 与状态栏逐行保存恢复是同类语义先例，可复用。

### 3.2 备用屏幕（alternate screen）语义

- 现状已有 `ui_backup[]` 整屏备份与状态栏恢复机制，但无"TUI 进场/退场"的统一约定。
- **借鉴**：定义进出场契约——进场备份整屏、清屏；退场恢复 `cells` 与光标位置；TUI 内容不进 scrollback（或入独立存档区）。CE ABI 以 append-only 槽位加 enter/exit TUI 接口，老二进制不受影响。

### 3.3 box-drawing 原语

- 现状：只有 ASCII `-`/`|`（fortune 框）；控制台 API 无 box 原语；Unicode 制表符（U+2500 系列）是否在位图表内需核对。
- **借鉴**：前端提供 `box(rect, style, single/double/rounded)` 原语；文本后端走制表符字形，gfx24 后端走 `gfx24_rect/pixel` 画线；`Block` 组件带标题区（对齐 ncurses box 与 Ratatui Block）。
- 前置：把制表符码点补进位图表（宿主工具烤入，复刻 `font_hz.inc` 生成流程）。

### 3.4 焦点管理与事件路由

- 现状：无控件概念，键盘事件由 shell/应用直读 `pollkey`/`getch`。
- **借鉴**：焦点栈 + 当前焦点控件；Tab/Shift-Tab 循环切换；焦点高亮（反色或强调边框）；事件路由——按键先送焦点控件，未消费再冒泡到容器/应用；模态弹出层独占焦点。
- 数据结构：控件矩形 + 焦点标记 + 事件回调表；CE 用户态可用函数指针表表达。

### 3.5 按键抽象增强

- 已有：>0xFF 功能键枚举、Ctrl+方向独立码、非阻塞/阻塞双读口。
- **缺口**：修饰键状态不随事件暴露；无统一事件结构；无 Resize/鼠标事件；未来接 USB/串口终端需解析转义序列。
- **借鉴**：对齐 ncurses keypad 与 crossterm Event 模型——统一 `KeyEvent { keycode, mods, ch }`；补 Resize 事件；修饰键状态在键盘层跟踪并随事件上报。

### 3.6 事件循环与渲染模式选择

- **建议选即时模式（Ratatui 式）**：每帧 = 读事件队列 → 更新应用状态 → 全量绘制到面板缓冲 → diff 提交。理由：无保留模式控件树的同步负担，契合 32 位小内存与现有双缓冲；ncurses 式多窗口脏矩形作为面板层优化后置。
- 循环原语：现有 `delay()` 睡眠 + `pollkey` 轮询；可演进为事件等待原语（与差距分析 §2.3 守护进程原语共用一套内核机制）。

### 3.7 约束式布局

- 已有 `con_cols()`/`con_rows()`/`con_info()` 尺寸查询。
- **借鉴**：Layout 分栏描述（水平/垂直、比例/固定/最小宽高），递归切出 Area 矩形；控件不自行计算绝对坐标；Resize 事件触发重布局。

### 3.8 文本样式模型

- 已有 `CON_ATTR_BOLD/DIM/ITALIC/UNDERLINE/BLINK/INVERSE/STRIKE/HIDDEN` 位；fg/bg 走 CLUT 索引或 RGB；256 调色板可写。
- **借鉴**：Span/Line/Text 分层（Ratatui）；样式随文本片段走，而非全局设一次写一段；颜色对映射纳入 CLUT。

### 3.9 鼠标（后续项）

- 现状：未发现鼠标驱动或鼠标事件通道。
- **借鉴**：PS/2 鼠标中断 → 事件队列（坐标 + 按键 + 滚轮）→ 控件命中测试（矩形包含）；先支持左键点击切换焦点与按钮，拖动/滚动后置。

## 4. TTF/OTF 轮廓字体渲染铺路

**现位图字体限制**

- 8×8 ASCII + 16×16 GB2312 固定点阵；无缩放、无抗锯齿；字符集受 Unifont 表覆盖；`render_font_glyph()`/`render_font_hz()` 按码点查表返回位图指针。

**轮廓字体管线要点**

1. 容器解析：TTF `glyf`（二次贝塞尔）/ OTF CFF（三次曲线）轮廓表；
2. 光栅化：扫描线填充或 SDF 距离场；hinting 网格对齐；
3. 位图缓存：码点 × 字号 × 样式 的字形缓存；
4. 排版：字距、行距、折行、双宽列宽处理。

**CatOS 约束**：32 位小内存、无 libc、内核/用户态分离、帧缓冲 supervisor-only（绘图经 syscall 进 `gfx24_*`）。

**分阶段路线**

- **阶段 1（今夜可做工具链储备）**：宿主侧预光栅化——把所需码点按目标字号/粗细烤成位图表，复刻 `tools/gen_hz_font.py` 流程；运行时只查表。先补 TTF 选字与位图导出工具链设计文档。
- **阶段 2**：运行时精简位图缓存（按字号缓存已光栅化字形，LRU 淘汰）；像素级字形经 `gfx24_blit` + alpha 混合上屏。
- **阶段 3（设计级）**：精简轮廓光栅器（仅 glyf 二次曲线、不全量 hinting）或 SDF 字形贴图；内存预算需先解 #128 第二段 RAM 枚举与 4G 档限制。

**与现有模型衔接**

- 文本前端单元格模型（8×16 逻辑格）与像素级字形的映射规则；CJK 双宽列宽（`disp_width` 已有先例）；反色/下划线等修饰在位图上的叠加方式。

## 5. TUI 视角差距清单（增量 / 设计级 / 暂缓）

| 项 | 分级 | 说明 |
|---|---|---|
| box 原语 + 制表符码点位图 | 可增量 | 前端 `box()` 接口；文本/ gfx24 双后端 |
| Span/Line/Text 样式分层 | 可增量 | 复用现有 CON_ATTR 位与 CLUT |
| 单面板缓冲抽象 | 可增量 | 全屏 TUI 起步，合成进 `cells` |
| 按键事件结构 + 修饰键上报 | 可增量 | `KeyEvent {code, mods, ch}` |
| alternate screen 进出场约定 | 可增量 | 复用 `ui_backup` 语义；ABI append-only 槽 |
| 多面板 z 序合成与遮挡裁剪 | 设计级 | ncplane/ panels 式平面栈 |
| 焦点管理与事件路由框架 | 设计级 | 焦点栈、回调表、模态独占 |
| 事件等待原语 | 设计级 | 与守护进程框架共用（差距分析 §2.3） |
| 鼠标驱动 + 命中测试 | 设计级 | PS/2 中断 → 事件队列 → 矩形命中 |
| 运行时轮廓光栅化 | 设计级 | 阶段 3，受内存模型前置约束 |
| 保留模式控件树 | 暂缓 | 即时模式够用后再评估 |
| 完整终端转义序列解析 | 暂缓 | 仅在接串口/USB 终端时需要 |
| 鼠标拖拽/滚轮完整支持 | 暂缓 | 左键点击闭环后再排 |

## 6. 参考来源

- ncurses 官方手册与 HOWTO：`stdscr`/`curscr` 双层屏、`WINDOW`/panels、`keypad()`、`box()`、颜色对、鼠标事件。
- Ratatui 官网（ratatui.rs）与 crates.io 文档：immediate-mode 渲染、`Layout` 约束分栏、`Widget`/`Frame`、`Text/Line/Span`、alternate screen + raw mode 进出场、事件由后端处理。
- notcurses 文档 / dankwiki：ncplane z 序叠加、行级 damage map、生成→blit 两阶段、24-bit RGB 与 EGC。
- Microsoft Learn《Edit command line text editor》；MS-DOS Editor 资料：菜单栏 + 编辑区 + 状态行、modeless、双 pane、字符画框。
- POSIX 1003.1 通用终端接口：前台进程组、特殊字符、终端访问控制（与差距分析 §2.4 作业控制互为参照）。
