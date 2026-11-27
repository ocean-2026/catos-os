# 参考实现研究：CatOS v0.15（1.44MB 软盘版）与 CDE

> 研究对象（外部文件，未入库）：
>
> | 文件 | 大小 | SHA-256（前 16 位） | 说明 |
> | --- | --- | --- | --- |
> | `catos.img` | 1,474,560 B | `6889a5ab00e09084…` | 1.44MB FAT12 软盘镜像，含 boot + loader + 平铺内核 |
> | `cde.c` | 115,444 B（2831 行） | `d7d39cb72e218359…` | 内核侧桌面环境 CDE v0.11 源码 |
> | `CDE_NOTES.md` | 40,290 B | `6cd963f7ee33e640…` | 18 章开发笔记（真 bug 的根因与修法） |
>
> 研究日期 2026-11-27。本文只记录"可迁移到本仓（D:\CatOs，v2.82）的部分"，
> 不复制第三方源码入库。

---

## 0. 一句话结论

这是一个**单软盘自举、内核内建桌面**的小内核（约 252 KB 代码 + 320×200×8bpp），
架构代差明显（我们已是 GRUB ISO + 800×600×24bpp + 声明式块树 + 画布通道），
**但它的三处工程手法和我们确有差距，值得抄**：

1. **菜单/浮层的字母加速键与"按键不穿透"**（我们只有 ↑↓/Enter/Esc）；
2. **点击在"控件内松开"才生效**（我们现在按下即触发，误触/误拖也会执行）；
3. **低分辨率 surface 按最近邻铺满任意窗口 + 指针逆映射**（我们的画布/图片是 1:1，
   320×240 的画布在 640×480 的块里只占左上角）。

其余多为"我们已有且更强"，或者"小内核的妥协，不该抄"。

---

## 1. 这份镜像到底是什么（实测）

### 1.1 引导与布局

BPB 实测（`catos.img` 偏移 11..28）：

| 字段 | 值 |
| --- | --- |
| OEM | `ForrestY` |
| 每扇区 / 每簇 | 512 B / 1 扇区 |
| 保留扇区 | **570**（291,840 B） |
| FAT 数 / 每 FAT 扇区 / 根目录项 | 2 / 7 / 224 |
| 总扇区 / 磁头 / 每磁道 | 2880（1.44MB）/ 2 / 18 |
| 卷标 | `Tinix0.01  FAT12` |

保留区 570 扇区里**平铺**着：boot sector（0x0000）、16 位装载器（0x1000 起，
可见 `B4 0E B0 45 CD 10` 的 BIOS 打字序列）、32 位 C 代码（0x8000 起，
典型 `55 89 E5` / `C9 C3` 的 gcc -m32 栈帧）、内建 8×16 点阵字模表，
末端（0x47000）恰好是编译器版本串 `GCC: (Rev4, Built by MSYS2 project) 16.2.0`，
紧跟其后就是 FAT 表（0x47400 起 `F8 FF FF …`）。

装载器串口/屏幕打点：`Booting Ready. No LOADER` → `[INFO] Entered protected mode`
→ `[INFO] Kernel loaded at` → E820 表（`BaseAddrL BaseAddrH LengthLow LengthHigh Type`）
→ `RAM size:`。也就是说：**自建两阶段自举 + 自己解析 E820**，不依赖 GRUB。

### 1.2 内核

- 32 位保护模式、平铺（非 ELF）、无 GRUB、无 ISO；
- 内核约 252 KB（0x8000–0x47400），含 IDT 异常名表、CDE、图形、FAT12、内存管理；
- 版本串 `CatOS v0.15 Pre 1`；
- 显示只有 320×200×8bpp（mode 13h）与可选 2× 缩放（640×400 需 SVGA）。

### 1.3 文件系统内容

根目录只有 `CATOS/`（目录）+ `TEST.ASM` + `TEST.C` + `HELLO.B`，
`CATOS/` 内只有 4 项：

```
CATOS/SYSSET.CON   233 B   # key=value 配置：dns= / theme=catos|dark|light / vmode=auto|vga|800x600…
CATOS/APP.CE      1924 B
CATOS/BF.CE       4056 B   # 4 KB 的 .ce，Brainfuck 解释器一类的小程序
```

**没有 desktop.ce** —— 桌面在核心里，`.ce` 应用靠 API 表（CE API 2.x）反向调用内核。
这是它和我们最根本的分歧：**它把窗口系统当内核子系统，我们把桌面当"块树 + 画布通道"的合成器**。

### 1.4 CDE 是什么

`cde.c` 头注释自述：*"CatOS Desktop Environment core (kernel side), v0.11"*，
拥有桌面、窗口、控件、菜单、鼠标光标与事件队列；GUI 应用**不许碰帧缓冲、不许碰 8042**，
只能通过 `cde_*` 建 UI、用 `cde_poll_event()` 收事件。规模：6~8 窗口、40~56 控件、
3 个菜单栏、每菜单 12 项、8 个桌面图标、12 个开始菜单项——全是编译期静态数组。

---

## 2. CDE 的做法（读码笔记）

### 2.1 三个"单一入口"设计（最值得学的方法论）

| 入口 | 作用 | 解决的问题 |
| --- | --- | --- |
| `dropdown_rect()` | 唯一计算下拉菜单位置/尺寸 | 绘制、点击命中、hover 命中三处各写一遍时，"贴边回退"只在绘制里做过 → 窗口靠下时点击位置对不上 |
| `menubar_title_x()` | 唯一计算菜单栏标题 x | 不再依赖上一次绘制留下的 `menu_x[]`（重排后过期） |
| `list_rows()` / `list_scroll_drag()` | 唯一按行高算可见行数 / 拖动映射 | 行高写死 10 与 `paint_list` 的 12 不一致 → 点选错行；滚动条滑块位置与拖动用不同 span → 拖不到底 |

同一条纪律：**任何"被画出来的几何"必须只有一个计算函数，绘制只消费它的输出**。

### 2.2 交互状态机（四件套）

```c
press_widget   /* 按下时记录；松开时若指针仍在控件内 → 发 CLICK */
scroll_drag    /* 列表滚动条 thumb 拖动（与 press_widget 互斥） */
drag_mode/drag_win  /* DRAG_MOVE / DRAG_RESIZE，松开才发 WIN_MOVED / WIN_RESIZED */
g_hover_widget /* 悬停；只有"指针真的移动"才让命中测试改写高亮 */
```

关键细节：

- **松开判定**：`release` 时重算控件内坐标，命中才发 `CDE_EVT_CLICK`，拖出只发 `MOUSE_UP`；
- **hover 不被静止指针覆盖**：记 `last_mx/last_my`，键盘移动菜单高亮后，只要鼠标没动，
  命中测试就不抢高亮（否则键盘高亮每帧被重置成 -1，看起来"没反应"）；
- **预填输入框的 `fresh` 标志**：首次输入字符替换默认文本（保存/重命名对话框语义），
  但退格/方向键仍编辑原文本；
- **点击定位 caret**：`caret=(cx-x-3)/8`，不做像素级字体度量也能对。

### 2.3 菜单语义（Win32 风格）

- 标签 `&New` 中的 `&` 是加速键，绘制时下划线、不显示 `&`；`menu_label_w()` 算宽度要跳 `&`；
- `Alt+字母` 打开对应菜单（菜单栏标题首字母），`kb_alt()` 由键盘层提供；
- 打开后**预选第一个可选项**（`menu_first_item`），↑↓ 跳过 SEP/DISABLED，←→ 换菜单栏，
  Enter 执行，Esc 关闭；
- 菜单打开期间**所有按键一律吞掉**（`k=0`），不许漏到背后的输入框；
- 右击不进菜单栏，只发 `CDE_EVT_RCLICK`（带命中的 widget/窗口/图标/列表行号），由应用决定弹什么。

### 2.4 guest 宿主窗口（把"全屏程序"塞进窗口）

```
cde_guest_begin(cw,ch,title)
  → 建宿主窗口；按 GFX_W*scale × GFX_H*scale 分配私有 surface（注释明确：
    2× 缩放下必须分配 640×400，否则越界写坏内核堆，症状是稍后"随机建窗失败"）
  → guest 的窗口/控件全部画到该 surface（compose 里分两段合成）
cde_guest_end()
  → guest_reap()：回收 guest 标记的窗口与控件槽
输入: 宿主客户区内 (mx,my) → (mx-cx0)*GFX_W/cw,(my-cy0)*GFX_H/ch
显示: guest_paint() 最近邻 (x*GFX_W)/cw 铺满宿主客户区
```

要点：**坐标空间隔离**（guest 用 320×200 逻辑坐标，宿主框架用屏幕坐标）、
**帧边界回收**（不是靠应用守规矩退出）、**缩放而非裁剪**（1:1 blit 会切掉右下角，
用户还得平移）。

### 2.5 渲染模型

- 整屏 `compose()` 一次性重画：壁纸 → 图标 → 桌面控件 → 窗口(按 z) → 下拉菜单
  → **任务栏（永远在窗口之上）** → 开始菜单 → 上下文菜单（最后，压过一切）；
- 软件光标"保存-还原"（`gfx_snapshot` + `gfx_bitmap`），不做光标图层；
- `screen_dirty` 只在真变化时置位；hover/press/select 各自只重画自己的控件；
- 图元在唯一出口 `px_run()` 里 `dirty_touch()`，合并成 pending 包围盒再 flush。

我们的层次表（`include/gui/block.h`）已经是更形式化的版本：
`BG(0) < CONTENT(100) < WORK(200) < CHROME(3000 任务栏) < OVERLAY(4000) < TOAST(5000)`，
且围栏模态用 `gui_modal_inert()` 而不是"z 高了就自然不吃点击"。

---

## 3. 与我们（CatOS 发行版 v2.82）对照

| 维度 | 参考版 v0.15 | 本仓 v2.82 | 判断 |
| --- | --- | --- | --- |
| 引导 | 自建 boot+loader，FAT12 软盘，自解 E820 | GRUB El Torito ISO（+ 可选磁盘镜像） | 我们更通用，**不换**；E820 自解析思路可作 ELF 以外的备用路径 |
| 内核布局 | 平铺 252 KB，无 paging（推断） | ELF/PIE 加载、分页、多任务、网络、TLS、包管理 | 代差 |
| 显示 | 320×200×8bpp（mode 13h）+ 2× | 800×600×24bpp，VBE dispi + VMware SVGA 双后端 | 代差 |
| GUI 模型 | 内核内建 widget 工具包 + 每窗口 kmalloc 客户缓冲 | 声明式块树 + 损伤区合成 + 画布通道（应用不碰帧缓冲） | 我们更现代 |
| 应用 | `.ce` + 静态 API 表（2.x） | ELF32/PIE CE + 69+ 槽系统调用 + 签名包 | 代差 |
| 文件系统 | FAT12，8.3 名，无 LFN | FAT32 + LFN + ACL + fsck | 代差 |
| 交互完备度 | 键盘可全操作菜单（`&`/Alt/↑↓/Enter） | 开始菜单可 ↑↓/Enter/Esc；**无字母加速、无 Alt 打开、浮层无键盘导航** | **参考版更好** |
| 点击语义 | 按下记录、**松开在控件内才算 click** | **按下即回调**（`input.c:308`） | **参考版更符合桌面惯例** |
| 画布/图片缩放 | 最近邻铺满窗口 + 指针逆映射 | 1:1 blit（`paint.c:886`/`890` 走 `gui_blit24*`） | **参考版有可借鉴点** |
| 光标 | 保存-还原 10×14 位图 | 保存-还原 + 被重绘单元自动补画（`v24_mouse_over_repaint`） | 我们更稳 |
| 输入队列 | IRQ12 + 环形队列，满则丢最旧 | 同（`mouse.c:139`） | 平手 |
| 字体度量 | 全部由 `UI_H` 派生（8×12 裁剪字库） | 8×16 点阵 + TTF，行高走布局层 | 我们更强；"单一度量常量"纪律仍适用 |

---

## 4. 值得汲取（按性价比排序）

### P1-1 菜单/浮层的字母加速键 + 首项预选 + 按键不穿透

**他们的实现**（`cde.c:563-590, 2836-2911`）：

```c
/* "&New" → 显示 New，N 下划线；菜单打开时按 n 直接执行 */
static char menu_accel(const char *s) { ... '&' 后一个字符，大写转小写 ... }
```

配合：`menu_first_item()` 打开即高亮；未匹配字母也 `k=0`（不进背后的输入框）；
`Alt+字母` 由 `kb_alt()` 打开菜单栏。

**我们的现状**：`chrome.c:879 menu_key()` 只有 ↑↓/Enter/Esc；`keyboard.c` 内部有 `alt`
状态（`keyboard.c:34`）但**没有导出访问器**；开始菜单标签是纯文本，没有加速键概念。

**落点（最小改动）**：

1. `include/keyboard.h` 增 `int kb_alt(void);`（一行返回 `alt`），`keyboard.c` 实现；
2. 菜单项标签允许 `&` 前缀（声明式侧可用 `accel` 字段走 append-only；先用 `&` 最省）；
3. `chrome.c` 打开菜单时扫一遍行标签建 `accel[]`（≤32 项，静态数组即可），
   `menu_key()` 的 `default` 分支查表命中即执行；未命中返回 1（吞键）；
4. 全局按键路径：`Alt+字母` → 打开对应顶层菜单（开始菜单先做，窗口菜单后做）；
5. 浮层（`popover.c`）补键盘：↑↓ 在子块间移动高亮、Enter 触发 `on_click`、字母直达。

**验收口径**：串口打点 `GUI:menu-accel PASS accel=n`；QEMU 键盘 `Alt+F` → 菜单开且首项高亮
→ 按 `N` 打开 Notepad；菜单打开时输入框收不到字符（打点 `GUI:key-swallow PASS`）。

**风险**：低。不动 ABI 结构体；`&` 若出现在纯文本标签需转义策略（只对菜单项解析）。

### P1-2 点击改为"按下+在控件内松开"

**他们的实现**（`cde_poll_event`）：按下只置 `pressed=1` + 记 `press_widget`，
松开时把屏幕坐标换回控件坐标，命中才发 `CLICK`，否则只发 `MOUSE_UP`。

**我们的现状**：`input.c:308` 在 `on_press` 里直接 `act->on_click(...)`；
`pressed_block` 只用于三态视觉（`input.c:276`）。副作用：

- 按下按钮后拖到别处再松手，动作照样执行；
- 触摸板/抖动误触概率偏高；
- 与"拖动标题栏"这类手势在同一按下帧里既触发 click 又启动 drag（靠 `resolve_interaction` 绕开，脆弱）。

**落点**：给 `block` 增 `click_on_up`（默认 1：按钮、菜单行、开始菜单行；0：画布、可拖动面板），
`on_press` 只记录 `armed_click = act`；`on_release` 里 `hit_test(x,y)` 命中同一 `act`
（或其子孙）才回调。拖动/缩放仍在 press 启动，但一旦进入 drag 就清 `armed_click`。

**验收口径**（三条探针，复用现有 `gui_inject_*`）：

| 手势 | 期望 |
| --- | --- |
| 按在按钮上 → 移出 → 松开 | 不触发 `on_click`，串口 `GUI:click-cancel` |
| 按在按钮上 → 原位松开 | 触发一次，`GUI:click-commit` 计数 +1 |
| 按标题栏拖动 → 松开 | 只发 WIN_MOVED/RESIZED，click 计数不变 |

**风险**：中。文件管理器行、菜单行、输入框都走 `on_press` 的同一路径，需要一轮
`gui` 全套回归（vprobe/E3/F11）+ 手感确认；可先只对 `BLK_BUTTON` 生效，观察一版再推广。

### P1-3 画布/图片按最近邻缩放铺满块（含指针逆映射）

**他们的实现**（`cde.c:1015-1035`）：

```c
int sy = (y * GFX_H) / ch; ... uint8_t px = srow[((x * GFX_W) / cw) * sc];
```

输入侧 `mx = (mx - cx0) * GFX_W / cw`（`cde.c:2321`）。

**我们的现状**：`cvchan.c:32-33` 画布上限 320×240；`cvchan.c:185` 逐行 1:1 拷进内核
backing；`paint.c:890` 用 `gui_blit24_stride` **按 1:1** 贴到 `b->x,b->y`。于是：

- 320×240 的画布放进 640×480 的块，只占左上角 1/4，没有"铺满"能力；
- 图片查看器（`images.c`）同样 1:1，大图只能裁、小图留白；
- 想做"把老式全屏程序装进窗口"的兼容宿主，缺的就是这一层。

**落点**：

1. `gui_blit24_scaled(dst_x,dst_y,dst_w,dst_h, src,src_w,src_h,src_stride, mode)`：
   纯整数最近邻（`sx = x*src_w/dst_w`，可预计算列索引表复用整行），
   避免浮点、避免每像素除法；
2. `paint.c` 的 `BLK_CANVAS` / `BLK_IMAGE` 分支增 `fit` 语义（`fit=none|contain|fill`，
   默认 none 保持现状）；
3. `cv_present_t` 不动（仍是源矩形），目标矩形由块的 `w/h` 决定 —— 这样 ABI 零变更；
4. 输入侧：`cap2p_interact.c` / 画布命中点按 `src = dst * src_w / dst_w` 反算，
   交给应用的坐标必须是源坐标（与 present 的空间一致）；
5. 配额：缩放不增加内存（backing 仍是源尺寸），只增加合成期 CPU；按目标面积计入
   单帧像素预算即可（当前 1/4 屏上限不变）。

**验收口径**：

- 320×240 棋盘格画布铺满 640×480 块：串口 `GUI:cv-scale PASS dst=640x480 src=320x240`；
- 采样对齐：目标 (0,0)/(639,479) 的取源像素分别为 (0,0)/(319,239)；
- 点击目标 (320,240) 反算源 (160,120)，误差 0；
- 帧时：800×600 下 320×240→全屏缩放合成 ≤ 8 ms（100Hz tick 打点）。

### P2-1 `/dev` 虚拟字符设备（我们完全没有）

他们的 `src/devfs.c`：`/dev` 是**真目录**，其下节点是虚拟设备；`fs_list / fs_foreach /
fs_resolve_file / fs_read_text / fs_write_raw / fs_cat / fs_entry_name` **七个入口**先查
devfs 再落回 FAT；`create_path / fs_remove / fs_copy` 对 `/dev` 一律拒绝写入。
设备：`null`（读空、写丢弃）、`zero`、`random`、`full`（写永远失败）、
`console/tty0`（写回显控制台+串口）、`version/hostname/uptime/meminfo`（只读文本）。

**价值**：`cmd > /dev/null`、`cat /dev/version` 这类 POSIX 脚本惯例立刻可用，
且给"能力位/设备"这类抽象一个真实挂载点。**成本**：一个 `devfs.c`（≈300 行）
+ 七处入口各加一个前置分支（`fs_resolve_file` 必须返回 `cluster=0`+虚拟 size，
否则 `fs_read_text` 会拿 0 号簇去读盘）。

**验收**：`ls /dev` 有节点且大小真实；`echo hi > /dev/null` 成功无文件；
`cat /dev/null` 返回 0 字节且**不算失败**（`n<0` 才是失败）；
`cp x /dev/full` 失败且不产生文件。

### P2-2 安装器的"名字 + 内容"双判据

他们的坑：安装器按内容哈希决定"要不要重写"，但旧版写坏的 8.3 短名（`STACKTES.CE`）
永远不被修；修法是把 **显示名** 一起带回（`fs_entry_name`）比对，不一致就
`fs_remove` + 重写，并给快速路径也加上名字检查。

**我们的现状**：`builtin_apps.c` 是"存在即跳过、永不覆盖"（自带注释
*"never overwrites user changes"*）。好处是不毁用户改动，坏处是**内核更新后旧内置
应用/文档永远是旧的**，且没有任何自愈路径。

**建议**：`/etc/builtin.ver` 记录"上次内核写入的版本 + 每文件内容哈希"；
启动时对**哈希未变（用户没改过）**的文件才允许覆盖；用户改过的保持不动并打点
`builtin: kept user-modified <path>`。这样既不毁改动，又能跟随内核升级。

### P2-3 窗口允许挂出屏（可选）

他们把"窗口必须完全在屏内"放宽为"标题条至少留 24×6 px 在屏内"，其余可以出屏；
我们 `input.c:329` 的 `BOUND_SCREEN` 是严格夹紧（整窗不得越界）。
800×600 下收益不大，但"窗口比屏幕还大"（如 900×700 的面板）时，严格夹紧会让
拖动出现"顶住边界"的僵硬感。可加 `BOUND_LOOSE` 档（保留标题条），默认仍 strict。

---

## 5. 他们踩过的坑 → 我们的核对清单

CDE_NOTES 里 18 章几乎全是"真 bug 根因 + 修法"，逐条对我们的代码做了核对：

| # | 他们的坑 | 根因 | 我们的状态 |
| --- | --- | --- | --- |
| 1 | 菜单命中与绘制位置不一致 | 几何在绘制/点击/hover 三处各写一遍，只有绘制做了贴边回退 | ✔ 我们 `pop_layout()` / `menu_open()` 各自单一入口；**新增子菜单时必须复用，不许再抄一份** |
| 2 | 键盘高亮每帧被鼠标重置 | 静止指针也在跑命中测试 | △ 我们 hover 只在"松开/移动"事件里更新（`input.c:405-409`）；**但焦点框 `focused` 与 hover 是两个状态，需确认二者不会互相覆盖** |
| 3 | "滚到底仍带选中条" | `list_sel<0` 被强制归 0 | △ `filemgr/list` 滚到底时首个可见行是否带焦点框，需实机看一次 |
| 4 | 后台输入框抢键盘 | 全局 `focus_widget` 不校验所属窗口 | △ 我们 `key_hook` 是全局单槽；`on_press` 点空白会清钩子（`input.c:296`），但**程序化 raise（`gui_focus_raise`）不清**，建议在 raise 里补清 |
| 5 | 事件环满 | 轮询 8042 在整屏合成时溢出 | ✔ `mouse.c` IRQ12 + 环形队列 + 丢最旧（`mouse.c:139`） |
| 6 | 退出码 350 被当"信号 222" | `rc>=128` 判信号 | △ 我们被杀是 `exit_code=-9`（`task.c:670`），可区分；但"程序主动返回 -9"会撞车。建议加 `killed` 标志位而非继续复用负值 |
| 7 | 旧 8.3 名永远不被修 | 安装器按哈希跳过，从不比对名字 | ✘ 我们更极端：存在即跳过（见 P2-2） |
| 8 | mode 13h 画面右移 160px / 512 行 | CRTC 表漏 2 个三元组，`CR0D=0x28`、`CR12=0xFF` | ➖ 不走 VGA 8bpp；但"寄存器表必须整段拷贝 + 回读校验"的纪律适用于 VBE/SVGA 任何模式表 |
| 9 | 8bpp 在 VMware 只有 1/4 亮度 | DAC 按 8bit 读，我们写 6bit | ➖ 24bpp 路径无关；我们 `be_vbe24.c` 已按 pitch/stride 处理，无"写死 320 步长"假设（`be_vbe24.c:337`） |
| 10 | 图形模式退出后字体坏掉 | SVGA 帧缓冲写坏 VGA plane 2 字库 | ✔ 我们有文本后端快照 + `restore_font` 同族机制；VMware 路径建议再跑一次 `text` 往返确认 |
| 11 | 反复开关窗口耗尽控件槽 | 40 个静态槽不复用 | ✔ 我们有 `b_remove` + `btable_alloc` 容量增长；**核对容量上限触发时的拒绝路径是否有打点** |
| 12 | 窗口客户缓冲越界写坏堆 | 2× 缩放时只分配 320×200 `guest_fb`，实际按 640×400 写 | ✔ 我们无每窗口 kmalloc 缓冲（统一合成背缓冲）；画布配额按面积硬限（`cvchan.c:105-113`） |
| 13 | 拖拽后按钮一直"按下" | 清 `pressed` 后忘了重绘 | △ 我们 `pressed_block` 由合成层统一重绘，风险低；P1-2 改造时注意"拖出取消"要触发一次重绘 |
| 14 | 列表点选错行 | 行高写死 10，绘制用 12 | ✔ `layout` 行高统一；核对 `filemgr.c` 的 `FM_ROW_H` 与字体行高同源 |
| 15 | 文本压到边框（"穿模"） | 未裁剪 / 度量没跟随字号 | ✔ 有 `ellipsis` 字符/词两级截断 + 探针（`cap2p_text.c`） |
| 16 | 焦点虚线穿过字形 | 8×8 时代的虚线画法 | ✔ 我们焦点用背景/边框反色，不画虚线 |

---

## 6. 不建议照搬

| 做法 | 原因 |
| --- | --- |
| 内核内建 widget 工具包（窗口/控件/菜单全在核心里） | 我们已经是声明式块树 + 用户态应用；回退等于把 GUI 重新焊死在核心里，违背"可拆卸桌面"路线 |
| 每窗口 `kmalloc` 客户缓冲 + `WIN_MAX_BUF=256KB` 手工夹取 | 我们有统一合成背缓冲 + 损伤区，无需每窗缓冲；他们的"分配量按 `scale²` 算"是一种昂贵的补丁 |
| 320×200 固定分辨率 + 15 色 + 3 菜单 / 6 窗口 / 40 控件的静态上限 | 纯属小内核妥协；我们的块表是动态容量 |
| `tag` 分段路由（Start 100–199、窗口菜单 500/520/540…） | 这是"事件不带来源 id"的补丁；我们 `block_id` 本身就是路由键 |
| 8×16 → 8×12 裁行做紧凑字库 | 只对点阵字库有意义；我们已有 TTF（`src/ttf.c`）与字号档位，别退回去 |
| 整屏 `compose()` 每帧重画 + 光标 save-under | 我们已经是损伤区合成；整屏重画在 800×600×24bpp 下代价太大 |
| 自建 boot/loader 取代 GRUB | 没有收益（多文件系统、多引导介质、可调试性都靠 GRUB）；E820 自解析可作为 GRUB 不可用时的备用路径记录 |

---

## 7. 复现与证据

镜像解析（宿主 Python，无需 qemu）：

```python
import struct
b = open("catos.img","rb").read()
bps,spc,rsv,nf,re,ts = struct.unpack("<HBHBHHH", b[11:11+11])
root = (rsv + nf*7 + re*32//bps) * bps     # 根目录偏移 = 0x49000
data = (rsv + nf*7 + re*32//bps) * bps     # 数据区起点
```

关键偏移（本镜像实测）：

| 偏移 | 内容 |
| --- | --- |
| `0x0000` | boot sector，`EB 3C 90` + `ForrestY` + `Tinix0.01  FAT12` |
| `0x1000` | 16 位装载器代码（BIOS 打字 + `int 13h` 读扇区） |
| `0x8000` | 32 位 C 代码（gcc -m32 栈帧；CDE/内核主体） |
| `0x46000` | 疑似内建点阵字模表（每行 4 字节、`6C 6C 6C 24 …` 的重复行模式） |
| `0x47000` | `GCC: (Rev4, Built by MSYS2 project) 16.2.0` |
| `0x47400` | FAT 表（`F8 FF FF …`），保留区到此为止 |
| `0x49000` | 根目录：`CATOS` / `TEST.ASM` / `TEST.C` / `HELLO.B` |
| `0x4AC00` | 数据区（`CATOS/SYSSET.CON` 等） |

字符串侧证：`[INFO] Entered protected mode`、`[INFO] Kernel loaded at`、
`BaseAddrL BaseAddrH LengthLow LengthHigh Type`、`RAM size:`、`CatOS v0.15 Pre 1`、
`#DE Divide Error … #XF SIMD Floating-Point Exception`（IDT 名表，Tinix 血统）。

---

## 8. 下一步建议

按投入产出排序，建议只先做 P1 三项（都可独立验收、都可回退）：

1. **P1-1 字母加速键**（0.5–1 天，改 3 个文件，无 ABI 变更）；
2. **P1-2 松开命中**（1–2 天，需一轮全量 GUI 回归；可只对按钮生效）；
3. **P1-3 缩放铺满 + 逆映射**（2–3 天，新增一个 blit 原语 + 两条命中路径）。

P2 两项（`/dev`、安装器双判据）建议排到 v2.83 之后的独立小版本，各自都有明确验收。
其余"坑清单"里的 △ 项，建议作为一次 30 分钟的代码走查任务，不单独开版本。
