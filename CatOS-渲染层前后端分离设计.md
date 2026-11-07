# CatOS 渲染层「前后端分离」设计说明（v2.7 + v2.8）

**分支**: `feat/render-split`（工作副本 `D:/CatOs-render-split`）
**基线提交**: `35de6c8`（2026-11-06 从共享工作树做的快照，含另一位工作者未提交的 v2.6 VMA/brk 改动与 2026-11-05 图形成果包）
**实现提交**: `5e55db1`（v2.7 前后端分离）；`v2.8` 见第九节
**原目录 `D:/CatOs` 保持不动**，仍是 `master` + 原来的未提交改动。
**状态**: 内核与 ISO 均编译通过；本机无 QEMU，**未做实机启动验证**。
第一至八节描述 v2.7；第九节是 v2.8 增量（中文 / 真彩 / 自定义调色板），
有冲突处以第九节为准。

---

## 一、结论先行

1. 渲染从其它层里彻底剥出来了：一个与模式无关的前端 + 两个可插拔后端。
   除 `src/render*.c` / `src/be_*.c`，全内核再无任何地方写 `0xB8000`、
   `0xA0000` 或 VGA 端口。
2. 图形模式（当前实现是 VGA Mode 13h，用户称之为 VBE 模式）**只做文本绘画**：
   40x25 字符格、256 色、8 种样式。所有帧缓冲图元都是 `static`，链接产物里
   不存在 `gfx_putpixel` / `gfx_fillrect` / `gfx_rect` 之类的符号。
3. 用户态**没有任何绘制 API**：没有绘图 syscall、没有 CE API 槽位、
   帧缓冲也没有映射进 ring3 页表（`task_space_create()` 里 PDE 0 是
   supervisor-only）。用户态能拿到的只有 `write/putc/cls/set_style/display_*`。
4. 用户态程序可以主动 文本→图形；图形→文本 **强制**经过一个物理 `Y` 按键确认，
   程序无法伪造（没有参数、没有 token、没有可跳过该路径的 syscall）。

---

## 二、架构

```
 shell / fs / net / intr / keyboard / builtin / CE syscalls
        |  vga_* (遗留兼容外观) 或直接用 con_*
        v
 +---------------------------------------------------------------+
 | [前端] src/render.c       include/render.h                     |
 |   - 逻辑单元格网格 80x25（char + fg + bg + attrs）            |
 |   - 光标 / 滚动 / 换行 / 当前样式                             |
 |   - 模式策略 + 用户同意门（唯一切换入口）                     |
 |   - 切换后端时重排(reflow)并重放网格，屏幕内容不丢             |
 +---------------------------+-----------------------------------+
                             | render_be_t (include/render_be.h)
              +--------------+----------------+
              v                               v
   [后端A] src/be_text.c              [后端B] src/be_gfx.c
   VGA text 03, 80x25, 16 色           Mode 13h, 40x25, 256 色
   只实现 draw_cell / clear /          只实现 draw_cell / clear /
   set_cursor / enter / diag           enter / diag
   拥有 0xB8000 + 文本寄存器           拥有 0xA0000 + 图形寄存器
                                      所有像素图元 static
              +--------------+----------------+
                             v
                     src/render_font.c（共享 8x8 点阵）
                             硬件

 [兼容层] src/vga.c : vga_* -> con_*（15 个瘦包装，让 ~150 个旧调用点零改动）
```

**关键取舍**：保留 `vga_*` 名字做兼容外观，不改 10 个既有调用文件。这样与另一位
工作者正在改的 `task.c`/`shell.c`/`ce*.h` 的冲突面最小。新代码一律写 `con_*`。

---

## 三、文件清单

| 文件 | 作用 |
|---|---|
| `include/render.h` | 前端契约：颜色、8 种样式位、模式、信息选择子、`con_*` API、策略常量 |
| `include/render_be.h` | 后端契约 `render_be_t`（纯绘制，无状态） |
| `src/render.c` | 前端：网格 / 光标 / 样式 / reflow / 模式切换 / 同意门 |
| `src/render_font.c` | 共享 8x8 点阵字库（两后端共用，其它层不需要） |
| `src/be_text.c` | 文本后端 + 03 模式寄存器序列 + 字形重载（QEMU 竖条纹修复保留） |
| `src/be_gfx.c` | 图形后端 + 13h 寄存器序列 + DAC + 样式化字符绘制（图元全 static） |
| `src/vga.c` | 兼容 shim，无任何硬件访问 |
| `include/vga.h` | 遗留名字映射 `VGA_* -> CON_*` |
| `src/chkstk.c` | MinGW gcc 16.x 的 `__chkstk_ms` 空实现（构建修复，见第六节） |
| `src/task.c` | 追加 3 个 syscall（枚举 49/50/51）、放宽 `set_fg` 到 0..255 |
| `include/ce.h` / `ce_abi.h` | ABI v2.7，49..51 槽位，追加式，旧二进制不受影响 |
| `src/shell.c` | `gfx` / `text` / `style` / `display` / `vdiag`，`color` 扩到 0..255 |
| `extapp/gfxdemo.c` | ring3 演示程序 `/bin/gfxdemo.ce` |
| `extapp/docs/API.txt` | v2.7 API 文档 |
| `build.sh`（根） | 改成委托 `kernel_v25/build.sh` 的入口；根级 `gfx.*/vga.c/shell.c` 是**冻结快照，不是构建输入** |
| `kernel_v25/build.sh` | 编译/链接列表加入 5 个新 .o，`gfxdemo` 加入 CE 构建列表 |
| `build/gen_builtin.py` | 加入 `/bin/gfxdemo.ce`；输出固定 LF，避免全文件 diff |

---

## 四、模式策略（第 2 条需求）

`con_request_mode(mode, origin)` 是**唯一**切换入口。

| 方向 | 谁可以 | 行为 |
|---|---|---|
| 文本 → 图形 | 内核 / shell / 任何 ring3 程序 | 直接切换，无需同意 |
| 图形 → 文本 | 任何人 | `con_ask_consent()` 打印提示并**阻塞读一个物理键盘按键**，只有 `Y`/`y` 放行，其它键拒绝并返回 `-13 (EACCES)` |
| 图形 → 文本（紧急） | 仅内核 `con_force_text()` | 跳过同意；panic/致命异常路径专用，无 syscall 可达 |

同意不可伪造的三点保证：
- syscall 只有 `int display_mode(int)`，参数只有目标模式，没有 consent 位；
- 同意来自 `kb_getkey()` 读 PS/2 扫描码队列，没有任何 syscall 能往队列写；
- origin 参数被故意忽略 —— 连 shell 的 `text` 命令也要按 `Y`，策略统一。

`display_mode(0)` 从 ring3 调用时会阻塞在 syscall 内（`task_block(T_WAIT_KB)`，
与现有 `SYS_GETCH` 同一条已验证路径），程序暂停，用户按键后继续。

---

## 五、图形模式 = 颜色和样式更多的文本模式

- 网格 40x25，字符 8x8，前景/背景各 0..255（DAC：0-15 标准色 + 16-255 灰阶）。
- 样式位：`BOLD 0x01 / DIM 0x02 / ITALIC 0x04 / UNDERLINE 0x08 / BLINK 0x10 /
  INVERSE 0x20 / STRIKE 0x40 / HIDDEN 0x80`。
  图形后端软件实现粗体（右移 1px 叠加）、斜体（上半部右移）、下划线、删除线、
  反显、暗色；文本后端只支持 BOLD/DIM/BLINK/INVERSE/HIDDEN（VGA 属性字节能力上限）。
  BLINK 当前只接受不动画（需要定时器重绘），已在文档标注。
- 文本模式遇到 >=16 的调色板索引时映射到最近的黑/深灰/浅灰/白。
- 切换模式时前端按新列数重排逻辑网格，屏幕文字不丢失。
- **没有**像素 API、没有矩形 API、没有演示绘图（旧的 `cmd_gfx` 像素 demo 已删除，
  换成纯样式文本欢迎页）。
- 图形模式无硬件光标（`set_cursor` 为 no-op）。

---

## 六、构建与验证

工具链：`/c/msys64/mingw32/bin`（gcc 16.2 / ld / objcopy），`PYTHON=python`。
`D:/Tools/MSY32/...` 那份缺 DLL，不能用。

```
cd D:/CatOs-render-split
PATH=/c/msys64/mingw32/bin:$PATH PYTHON=python sh build.sh iso
```

验证结果：
- `kernel.bin` 5.1 MiB、`catos.iso` 16 MB 均生成成功，0 error / 0 undefined reference。
- `nm build/kernel.pe`：前端 `con_*`、后端 `be_text`/`be_gfx` 正常导出；
  **没有任何 `gfx_putpixel`/`gfx_fillrect`/`gfx_rect` 符号**。
- vsyscall 页：52 个 thunk 共 1144 字节 < `VS_EXIT_STUB` 的 0x500(1280)，未溢出；
  52 个指针表 208 字节位于 0x800，布局安全。
- 全树 grep：`0xB8000`/`0xA0000`/VGA 端口只出现在 `be_text.c`、`be_gfx.c`。
- `builtin_apps.c` 用重新生成的 `.inc` 单独编译通过；`gfxdemo.ce` 头 `CE01`，
  image 4636 字节。
- 构建修复：MinGW gcc 16.x 会给 `fs.c` 的大栈帧生成 `__chkstk_ms` 调用，基线内核
  在链接期报 `undefined reference`。新增 `src/chkstk.c`（与 `extapp/ce_stub.c` 同理的
  空实现）。这不是本需求的改动，但不加就无法编译验证。

**未验证**：本机没有 `qemu-system-i386`，所以 Mode 13h 寄存器序列、DAC 调色板、
字形重载、同意门的按键交互**都还没有实机跑过**。合并到主线前必须在 QEMU 里走一遍：
`gfx` → `gfxdemo` → `text`（按 Y / 按 N 各一次）→ 再 `gfx` → `text` 多轮往返。

---

## 七、已知限制

1. 图形模式 40 列，文本模式 80 列。切换时 reflow 会重排并可能丢掉超出新行数的内容；
   `edit` 全屏编辑器按 80 列写，在图形模式下只显示左半边。
2. 图形模式无光标、无双缓冲、BLINK 不动。
3. 同意门在「后台任务 + shell 同时在读键盘」时可能与 shell 抢按键；建议用前台任务
   调用 `display_mode(0)`。要彻底解决需要给同意门加独立的控制台所有权协议。
4. `con_force_text()` 没有调用方，只是给未来 panic 路径留的内核后门。
5. 根目录 `gfx.c/gfx.h/vga.c/shell.c` 是 2026-11-05 的冻结快照，**不参与构建**；
   单一源码事实是 `kernel_v25/`。合并回主线时建议把这几个根级文件归档或删除。

---

## 八、合并回主线的建议

- 本分支改动面（相对基线）：11 个已跟踪文件 + 9 个新文件，`+757 / -207`。
  已刻意还原被重编的 10 个既有 `.ce` 二进制，避免与另一位工作者的 WIP 撞车。
- 真正与他人工作重叠的文件只有 `src/task.c`、`src/shell.c`、`include/ce*.h`、
  `include/vga.h`、`src/builtin_apps.inc`、`build.sh`、`gen_builtin.py`；
  合并时优先按「追加式」处理（syscall 枚举/ABI 槽位都在尾部）。
- 建议顺序：先在 QEMU 验证 → 再把分支 cherry-pick / merge 回 `D:/CatOs` 的
  一个新分支 → 与另一位工作者的 v2.6 VMA 改动联调。

---

## 九、v2.8 增量：中文显示 + 真彩文字 + 自定义调色板 + VBE 后端

用户给了两条路：① 中文支持；② "255×255×255 色或其它多色"。结论是
**① 是真需求，② 的前提是错的**，但按"全做"的要求，② 被拆成两个
可交付物一并实现。

### 9.1 为什么 "255×255×255 色" 在 Mode 13h 上不成立

- VGA Mode 13h 是 **8bpp 索引色**：帧缓冲每像素 1 字节，同屏最多
  **256 色**。这是硬件上限，不是软件偷懒。
- VGA DAC 每通道 **6 bit（0..63）**。v2.7 的 `be_gfx.c` 往 `0x3C9`
  写 0..255 会被硬件截断成 0..63 —— 连"255 级/通道"都不存在。
  v2.8 已改成显式 `rgb >> 2`。
- 真正的 1677 万色必须换 **24/32bpp 线性帧缓冲**，那是一次底层重写。
- 更根本的：图形模式被定位成"颜色和样式更多的文本模式"，一次只用
  前景+背景两色，256 种可选色已经过剩。1600 万色对纯文本收益接近零。

因此 ② 落成两件事：
1. **务实版**：用户态可自定义 256 项 CLUT（Mode 13h 同步写 6-bit DAC）；
2. **真彩版**：新增 VBE 32bpp 后端，每个字符用字面 RGB 绘制。

### 9.2 中文显示

| 项 | 实现 |
|---|---|
| 字库 | GB2312 全 **6763 字**（U+4E00..U+9FA0），16×16 1bpp，32 字节/字 |
| 生成 | `tools/gen_hz_font.py`，用本机 `simsun.ttc` 16px + Pillow 渲染；产物 `src/font_hz.inc`（750 KB 源码 / 216 KB 数据）**已提交**，构建不依赖 Pillow 或字体文件 |
| 查找 | 按 Unicode 码点排序 + 二分查找，`render_font_hz(cp)` |
| 编码 | `con_write()` 内置 UTF-8 解码状态机；每次 write 结束 `con_flush_utf8()` 把残字节降级为 `?`，杜绝多任务串扰 |
| 排版 | 汉字占 **2×2** 个 8×8 逻辑格；强制从偶数行起，滚动固定 2 行；`reflow()` 把 2×2 块当原子搬运，切模式不会把汉字切一半 |
| 覆盖 | 文本模式不支持（`cjk=0`，降级 `?`）；图形 / VBE 完整支持 |
| 输入 | **没有中文输入法**，键盘仍是 US QWERTY 扫描码。只能显示 |

字库由系统字体渲染生成，本地 / 私有使用没问题；**对外分发内核镜像前
需确认字体 EULA**。

### 9.3 颜色模型升级

前端 cell 从 `(ch, fg8, bg8, attrs)` 升级为：

```c
typedef struct {
    uint32_t cp;                  /* Unicode 码点，0 = 空 */
    uint32_t fg, bg;              /* 0x00RRGGBB */
    uint8_t  fg_idx, bg_idx;      /* 量化后的 CLUT 索引 */
    uint8_t  attrs, flags;        /* 样式 / 宽字符标记 */
} con_cell_t;                     /* 16 字节 */
```

- 前端持有 256 项 CLUT；`set_style(int,int,int)` / `set_fg(int)` 保持
  **索引语义**（旧二进制不变），新增 `set_style_rgb` 走字面 RGB。
- 三后端各自翻译：`be_text` → 最近 16 色；`be_gfx` → CLUT 索引；
  `be_vbe` → 直接写 32bpp RGB（真彩）。
- `set_palette(idx, rgb)` 改 CLUT 并同步 6-bit DAC，然后重画整屏。

### 9.4 第三个后端：VBE 800×600×32

`src/be_vbe.c`，100×75 逻辑格，`colors=16777216`，`bpp=32`。

- 模式设置走 **Bochs/QEMU VBE dispi 端口** `0x1CE/0x1CF`，不需要在
  保护模式里调 BIOS `int 0x10`。
- LFB 物理地址来自 VGA PCI BAR2（QEMU std VGA `1234:1111`、QXL、
  Virtio GPU），老 Bochs 回退 `0xE0000000`。
- 新增 `vbe_lfb_map()`（`paging.c`）：为高端 PDE 建 4 KiB 页表，
  **supervisor-only**，且拒绝触碰 PDEs 0..15。ring3 页目录只复制前
  16 个 PDE，所以真彩模式同样看不到帧缓冲 —— "不开放绘图 API"
  在 32bpp 下依然成立。
- 硬件探测失败时 `enter()` 返回负值，`con_request_mode(VBE)` 返回
  `-19 (ENODEV)` 且**不改变当前模式**。

### 9.5 模式策略更新

`con_request_mode()` 仍是唯一入口：

| 方向 | 规则 |
|---|---|
| → GFX / → VBE | 所有来源（含 ring3）直接放行 |
| 任何非 TEXT → TEXT | **一律**要物理 `Y` 确认，origin 参数被忽略；连 shell `text` 也要按 Y |
| `con_force_text()` | 内核紧急路径专用，无 syscall 可达 |

`switch_backend()` 顺序改为 **先 `old->leave()` 再 `new->enter()`**：
VBE 的 dispi 关闭必须先于 Mode 13h / 文本模式的寄存器序列，否则会
被后续 disable 重置。探测失败会调 `old->enter()` 恢复原模式。

### 9.6 CE ABI v2.8（55 槽，追加式）

| 槽位 | 调用 |
|---|---|
| 52 | `void set_style_rgb(uint32_t fg, uint32_t bg, int attrs)` |
| 53 | `int set_palette(int idx, uint32_t rgb)` |
| 54 | `uint32_t palette(int idx)` |

`display_mode()` 增加 `2 = VBE`；`display_info()` 增加
`7=bpp / 8=CJK / 9=字库字数`；`display_mode(0)` 从 VBE / GFX 回文本
同样要 Y。旧 `.ce` 二进制不受影响。

`vsyscall` 页现在有 **55 个 thunk = 1216 字节**，距 `VS_EXIT_STUB`
的 `0x500`(1280) 只剩 **64 字节余量（约 2 个 syscall）**。
已在 `build_vsyscall_page()` 加溢出守卫：一旦越界会打印并停机，
不再像以前那样静默破坏 `ce_main` 的 return 蹦床。再加 syscall 前
必须重排 vsyscall 页布局。

### 9.7 构建与验证（v2.8）

- `sh build.sh kernel` / `sh build.sh iso` 均通过，0 error。
- `kernel.bin` 5.78 MiB（v2.7 是 5.1 MiB，+~670 KB：字库 216 KB、
  双份 100×75 网格 240 KB、VBE 后端与前端代码）。
- `nm build/kernel.pe`：三后端 `be_text/be_gfx/be_vbe` 均在，
  **没有任何绘图符号导出**。
- 二进制抽查：`zhdemo.ce` 含 `你好，世界！` 的 UTF-8 字节；
  `kernel.bin` 含「中」的 16×16 点阵。
- 工具链 `/c/msys64/mingw32/bin`；`D:/Tools/MSY32/...` 缺 DLL 不可用。
- **未验证**：本机没有 `qemu-system-i386`。VBE dispi 寄存器序列、
  PCI BAR2 取值、LFB 页表映射、中文 2×2 排版、UTF-8 解码、同意门
  按键交互**全部没在实机跑过**。合并前必须在 QEMU（建议
  `-vga std -m 256`）里走：
  `gfx` → `zhdemo` → `vbe`（看是否报 ENODEV）→ `text`（Y / N 各一次）
  → 再 `gfx` → `text` 多轮往返，并确认无竖条纹、无汉字错位。

### 9.8 已知限制（v2.8 新增）

1. 文本模式中文降级 `?`；VGA text 的字形区只有 256 个 8×16 槽，
   要做混合宽度中文需动态改字形寄存器，本期不做。
2. 汉字强制偶数行 + 滚动 2 行，纯 ASCII 输出滚屏时会一次跳 2 行。
3. `BLINK` 仍只接受不动画。VBE 无硬件光标。
4. VBE `redraw_all()` 一次约写 1.9 MB，滚动可能明显慢，后续需要脏矩形。
5. 编辑器按 80 列写 `vga_putc_at`，在 VBE 模式只占左上 80 列；
   覆盖汉字格会调用 `break_block()` 清掉整个 2×2 块。
6. `SYS_WRITE` 单次 512 字节上限未动（约 170 汉字）。
7. 字体衍生点阵的授权见 9.2。

### 9.9 基线同步记录

另一位工作者 18:39 在主目录加了 `kernel_v25/src/stubs.c`（内含
`__chkstk_ms`）并把它挂进 `build.sh`。v2.7 分支里的 `src/chkstk.c`
定义了同一符号，合并会重复定义。本次已：

- 复制 `stubs.c` 到隔离副本、删除 `chkstk.c`、
  把副本 `build.sh` 的 `chkstk.c/chkstk.o` 换成 `stubs.c/stubs.o`；
- 核对了两份 P0 报告（VMA / 经验教训），未纳入本分支以免合并重复；
- 确认 `task.c` / `ce.h` / `ce_abi.h` 与基线一致（他没动），
  所以他的 v2.6 P0 改动和我的 ABI 追加不冲突。

---

## 十、v2.9 增量：清掉 v2.8 的全部遗留限制（除实机测试）

用户要求"你说的那些没做到的你能不能解决（测试免了）"，本节逐条对应
v2.8 最终回复里列出的限制。仍然**未做 QEMU 实机验证**（用户明确免除）。

### 10.1 逐条清单

| v2.8 的限制 | v2.9 的解法 |
|---|---|
| 字库来自 simsun，对外分发有 EULA 风险 | 全部 6763 个点阵换成 **GNU Unifont 16.0.01**（GPLv2+ 字体嵌入例外 / SIL OFL 1.1 双许可），提交 `tools/unifont-gb2312.hex` + `UNIFONT-LICENSE.txt` + `OFL-1.1.txt`；与旧数据逐字比对仅 52 字有渲染差异，「中」完全一致 |
| 汉字占 2x2、强制偶数行、滚屏一次跳 2 行 | 单元格统一为 **8x16**，汉字 = **2 列 x 1 行**；滚动一律 1 行，reflow 不再有奇偶规则 |
| VGA 文本模式中文降级 `?` | 文本模式用 VGA 备用字形槽 **0x80..0xFF** 动态装载 8x16 半形，同屏 64 个不同汉字；图形/VBE 无上限 |
| VBE 滚动整屏重绘 1.9 MB | 后端新增 `scroll()`：帧缓冲前向搬移，一行 51 KB；`redraw_all()` 跳过已被 clear 覆盖的空白格 |
| BLINK 只接受不动画 | PIT 100Hz tick 驱动 `con_tick()`，0.5 s 翻相，只重绘闪烁格与光标格；文本模式走硬件闪烁位；IRQ 重入用 `render_busy` 栅栏防撕裂 |
| 图形/VBE 无光标 | 软件下划线光标 `draw_cursor()`，由前端统一擦/画，`con_status_bar` 与闪烁共享同一条路径 |
| `SYS_WRITE` 512 字节上限 | 256 字节分块循环，上限 64 KiB；逐页 `validate_uaddr`（含按需映射）；`con_write_n(s,n,final)` 让 UTF-8 残字节跨块存活，块间不清码器，任务间仍不串扰 |
| 编辑器硬编码 80x25 | `ed_view_rows()/ed_max_cols()` 跟随后端行列；内容行用 `con_write_at_utf8`，能显示文件里的中文 |
| vsyscall 页只剩 64 字节余量 | 重排：thunk 区扩到 0x000..0x7FF（2048 B，余量 **832 B ≈ 34 个 syscall**），API 表 0x800、ls thunk 0xC00、exit 蹦床移到页尾 0xF00；旧 `.ce` 不直接引用这些地址，内核 spawn 时写入，二进制兼容 |
| 没有中文输入法 | 新增拼音 IME：F2 开关，字母拼 → 1-9 选项 → 空格首选 → `-`/`=` 翻页；候选按 jieba 词频排序（400 音节 / 6763 字，91 KB 提交表 `src/pinyin.inc`）；候选栏画在底行且**原样保存/恢复该行**，不破坏 shell 输出；shell 行编辑与 `edit` 编辑器都接入 |

### 10.2 关键实现位置

- `render.c`：前端全部重写（`paint/sync_cursor/scroll_one/hz_rebuild_map/
  reflow/con_write_n/con_tick/con_status_bar*`）。
- `render_be.h`：`con_cell_t` 增加 `gslot`；`render_be_t` 增加
  `scroll/draw_cursor/sw_blink`，NULL 一律安全降级。
- `be_text.c`：`font_begin/font_end/font_put` 抽出，`text_load_hz(cp,slot)`
  带缓存装载半形；`sw_blink=0`（硬件闪）。
- `be_gfx.c` / `be_vbe.c`：8x16 绘制 + `scroll` + `draw_cursor`，
  `sw_blink=1`。
- `ime.c` + `pinyin.inc`：独立模块，`kb_readline` 与 `cmd_edit` 各接一个
  `ime_feed` 调用点。
- `task.c`：SYS_WRITE 分块；vsyscall 布局。
- `intr.c`：IRQ0 里 `con_tick()`（在 `task_tick_wake` 之前）。

### 10.3 新的已知限制

1. 文本模式同屏 **64 个不同汉字**上限（字形槽只有 128 个），超出降级 `?`；
   图形 / VBE 无限制。
2. 候选栏占用底行；IME 激活期间该行内容被暂存，关闭时原样恢复。
3. IME 是单字输入（无词组联想）；多音字按 jieba 频率排序，冷僻音节可能
   要翻页。
4. VBE `scroll` 每次仍要搬 51 KB/行（37 行满屏 ≈ 1.84 MB 的 32 位拷贝），
   比逐格重绘快约一个数量级，但还没有做 dispi Y_OFFSET 硬件平移。
5. 编辑器缓冲仍是 64 行 x 99 字节（约 33 个汉字/行）。
6. `SYS_WRITE` 单次上限 64 KiB（防呆，不是流接口）。
7. QEMU 实机验证依旧未做（本轮明确免除）：文本模式字形槽装载、VBE 滚动、
   BLINK 相位、IME 候选栏、SYS_WRITE 分块都只是编译级验证。

### 10.4 v2.9.1：VBE 性能（MTRR WC + 硬件平移滚动）——含审问记录

针对"VBE 慢"做了三层优化，并对计划做了逐分支审问（grill-me），决策记录
在此。**MTRR / 硬件平移部分仍未实测**（QEMU 清单见 10.4.4）。

#### 10.4.1 实现

1. **MTRR Write-Combining**（`be_vbe.c: mtrr_set_wc`）：按 SDM 完整序列
   （CD|NW → wbinvd → MTRRs off → 编程 → MTRRs on → wbinvd → 恢复 CR0/
   EFLAGS），为 LFB 找一个空闲变量 MTRR 标成 WC。护栏：只对
   `>= 0xC0000000` 的区域生效（绝不覆盖系统 RAM）；区域必须是 2 的幂且
   基址对齐，对不齐则向下对齐并扩区域（QEMU 下无害，真机上可能覆盖其它
   BAR——已知瑕疵，真机线下再修）；CPUID 检查 MTRR 支持；失败一律静默
   退回 UC。
2. **硬件平移滚动**：按 BAR 实际大小申请虚拟高度（上限 4096px = 256 行），
   滚动 = 先把新露出的一行擦成背景色（12.8KB 纯写）再写 DISPI_Y_OFFSET，
   **零像素搬运、零 VRAM 读**。视口到底时回卷到 0 并让前端整屏重渲染
   （约每 219 次滚动一次 1.9MB 纯写突发）。
3. **明确不做像素双缓冲**：前端的单元格网格就是权威屏幕副本，所有像素
   都由它派生，1.9MB RAM 镜像买不到任何东西。退化路径 = 前端从网格整屏
   重渲染（纯写），替代旧版的"显存 memmove"（UC/WC 下读显存都是毒药）。
4. 退化可观测：`vdiag` 与 `display` 都输出 `MTRR-WC/UC` 与
   `HW-pan/re-render` 的实际生效状态（CON_INFO_WC=14 / HW_SCROLL=15）。
5. 修复自查发现的 bug：VCNT 应从 MTRRcap(0x0FE) 读，v2.9.1 初版误读
   MTRRdefType；defType 恢复时保留固件原默认类型而非强写 WB。

#### 10.4.2 审问决策（2026-11-07）

| 分支 | 决策 |
|---|---|
| 目标平台 | **QEMU 优先，VMware 次之（用户亲自测），真机尽力而为**；只留三个廉价护栏 |
| VMware | **接受无 VBE**：VMware SVGA II 不实现 dispi 端口，`vbe` 会返回 -19；SVGA II 原生后端记为将来可选，不写没环境验证的代码 |
| 滚动权衡 | **wrap 突发 / 无 vblank 同步 / 每行 12.8KB 擦除三个全接受**；不引入 vblank IRQ 或轮询，不上像素双缓冲 |
| 合并标准 | **允许合回主线，提交信息带"MTRR/pan 未实测"标记**，实测清单过后摘除 |
| 路线 | **深度优化 VBE 为最终路线**；GOP / 专有驱动 / 退回文本模式排除；**GOP 保留为将来可选后端**（render_be_t 插拔结构已支持） |
| 真机线下已知瑕疵 | BAR 探测未先关 PCI memory decode；WC 区域扩大可能覆盖同区段其它 MMIO BAR |

#### 10.4.3 性能账

| 操作 | v2.9（旧） | v2.9.1（新） |
|---|---|---|
| 滚动一行 | VRAM 读+写 memmove ≈ 3.8MB | **12.8KB 纯写 + 一次 I/O 写**（HW-pan 生效时） |
| 滚动一行（pan 不可用） | 同上 | 1.9MB 纯写重渲染（仍无 VRAM 读） |
| LFB 写带宽 | UC，每 4B 一次 PCI 事务 | WC，写合并（真机约 10-20x） |
| 整屏 clear | 1.9MB 写 | 1.9MB 写（不变，纯写） |

#### 10.4.4 QEMU 实测清单（摘除"未实测"标记的唯一标准）

1. `vbe` 进出各两次：画面无错位、无竖条纹；
2. 连续滚屏 >500 行（跨越 wrap ≥2 次）：无内容丢失；
3. `vdiag` 确认 `mtrr=WC` 且 `scroll=HW-pan` 生效（而非静默降级）；
4. BLINK 动画与软件光标叠加正常；
5. `text` 同意门照常（按 Y/N 各一次）；
6. （可选）VMware 里确认 `vbe` 返回 -19 且 text/gfx 照常。

---

## 十一、v2.10 增量：TD-01 修复、mmap、ELF 加载器、CE 回归套件

本轮完成 P1 待办四项（QEMU 实机测试见 11.5）。P2（fork/COW、信号、
musl、VFS、设备文件）不在本轮范围。

### 11.1 TD-01：page fault 致命路径（真 bug，比待办描述的更糟）

`vma.c: task_page_fault()` 致命路径打印 kill 横幅后直接
`__builtin_unreachable()` —— **没有 return -14**。intr.c 的
`isr_exception` 兜底（-14 → task_die_fault → task_cleanup + zombie +
reschedule）永远走不到，控制流落入未定义行为。

修复：vma.c 删除打印与 unreachable，直接 `return -14`，复用 intr.c
现成的 `task_die_fault()` 兜底。注释里所谓"环形依赖无法调用 task.c"
不成立（task.c 是调用方）。顺手删除 vma.c 三个无人调用的打印助手。

### 11.2 mmap / munmap（CE ABI v2.10，槽位 55/56）

**地址空间扩容**（前提）：原内核用户空间只有一张用户页表（PDE 8，
32-36 MiB），CE 全家桶占了大半。新增第二个用户 PDE（9 号，36-40 MiB）：
- `task_space_t` 增加 `pt2`；`task_space_create` 挂两张用户页表，
  失败路径不再泄漏帧（原代码部分失败会漏）。
- `task_map_page / task_virt_to_phys / 新增 task_unmap_page` 按 PDE
  分派；`task_space_destroy` 释放两张页表的全部帧。

**语义**：
- `mmap(addr, len, prot, flags)`：addr 必须 0；匿名映射，first-fit
  分配在扩展区；每映射前面带 8 KiB guard 页（返回地址在 guard 之后，
  越界下写直接 page fault 杀任务而不是踩邻居）；页面 lazy 映射
  （复用 page fault 路径）；返回用户地址或 -22/-12。
- `munmap(addr, len)`：解除映射、释放帧、归还空洞；部分解除缩小 VMA，
  全解除删除 VMA。只接受本任务 mmap 出来的区间（-22 拒绝其它）。

### 11.3 ELF32 加载器（PIE + ASLR，双轨制第三轨）

`task_spawn_ce` 现按 magic 分派：`CE01` 走旧路径（PDE 8 固定偏移），
`\x7fELF` 走 ELF 路径（PDE 9 扩展区）。

- 校验 ELF32 / LE / i386 / ET_DYN / PT_LOAD 数 ≤16 / 文件 ≤2 MiB。
- **ASLR**：PIE 基址 = USER_EXT_BASE + (tick 哈希 mod 16) 页，每次
  spawn 滑动 0-64 KiB。
- 段加载：PT_LOAD 页区间排序合并（经典 RO 尾页 / RW 首页共享页），
  逐合并区间 kzalloc + map + 建 VMA；再按段从 FAT 直接读文件字节进
  用户页（`fat_pread` 支持任意文件偏移跨簇读）；bss 由零页天然覆盖。
- 入口 = base + e_entry，校验落在已映射区间。
- **syscall 约定**：ELF 没有 __ce_api 槽，直接调固定 vsyscall 表
  `CATOS_VS_TABLE`（0x2080800，ce_abi.h 新宏）：
  `call dword [CATOS_VS_TABLE + 4*slot]`。
- ET_EXEC 也按 PIE 处理（文档明说：仅支持位置无关代码）。

**工具链现实**：本机 MinGW 无 elf_i386 输出能力（连 MSYS2 usr/bin 的
ld 都只支持 pe）。`tools/mkelf.py` 手工构造最小但真实的 ELF32
（一段 RW PT_LOAD，机器码调 vsyscall write+exit），产物
`extapp/bin/hello.elf` 已编入内核内置表，QEMU 里 `hello.elf` 即可
验证 ELF 路径。**没有真实 C→ELF 编译链**——这是后续待办（binutils
elf 目标或 lld）。

### 11.4 CE 回归套件（tests/ce_regress.py，宿主零依赖）

- 跑 extapp/bin/*.ce 全部 18 个镜像过 `tests/ce_sim.py`。
- PASS=5 / SKIP=13 / FAIL=0。SKIP 全部有因：交互 API（readline/getch）
  ce_sim 不建模、历史未建模指令（test_ab 的 group3/mul）。
- **回归抓出两个真问题**（这正是建套件的意义）：
  1. `ce_sim.py` API 表停在 v2.5（48 槽），thunk 识别窗口硬编码
     64 字节 —— 已扩到 v2.10（57 槽）+ 修窗口 + 宿主侧实现
     mmap/munmap（first-fit 与内核语义对齐）。
  2. `cc.c / cc_self.c`（cc.ce 编译器）API 表同样停在 48 槽，
     `api->mmap` 会编出错误索引 —— 已补齐 57 槽（cc.c 数组 +
     cc_self.c 的 g_apin[57]）。调用点的 `slot < 0` 防呆原本就有，
     旧状态会报 "unknown api function" 而不是静默错。
- `tests/qemu_regress.py`：QEMU monitor（TCP 4444）+ sendkey +
  screendump 驱动端到端冒烟：boot/help/display/gfx/text 同意门
  （Y 拒绝与接受各一次）/mmaptest/hello.elf/vdiag，截图到
  tests/qemu_shots/，自动断言帧非黑与帧变化，文本级人工看图。

### 11.5 QEMU 实机测试：未完成（网络）

本机无 QEMU。winget 源状态损坏（卸载后仍报"已安装"）；官方
qemu-w64-setup-20260811.exe（160 MB）直连下载 ~25 KB/s，多轮断点
续传仍未完成。**qemu_regress.py 已就绪，装好后一条命令可跑**。
v2.9.1 的 [MTRR/pan UNTESTED] 标记继续保留。

### 11.6 已知限制

1. ELF 仅支持位置无关代码；没有动态链接、没有 symbol relocation。
2. munmap 空洞归还后，mmap first-fit 会复用地址（测试利用了这点）；
   无 ASLR 重随机。
3. 扩展区共 4 MiB（单页表上限）；ELF 与 mmap 共享，ELF 最多 2 MiB。
4. `fat_pread` 与 spawn 共用 static 簇缓冲（ring0 非重入，安全）。
5. cc.ce 尚无 `void*` 指针运算的全部语义，mmaptest 只能用 gcc 构建。

### 11.7 QEMU 实机测试结果（2026-11-07，QEMU 11.1.0 @ C:/qemu-portable）

QEMU 经由官方安装器 7z 解包为便携版安装成功。`tests/qemu_regress.py`
端到端跑通：**12 步 12 PASS**（boot/format/help/display/gfx/同意门
N+Y/ls/mmaptest/hello.elf/vdiag/final-frame），截图在
`tests/qemu_shots/`。实机验证确认并修复了 **5 个只有真机才暴露的 bug**：

1. **字形重载写错了地方（v2.9 的原始 bug）**：`fontwin` 停在 0xB8000
   且从不切窗 —— GC 0x06=0x0E 时 0xA0000/B8000 都别名到文本缓冲，
   "字形装载"实际是把 8KB 字形喷进文本缓冲，字形 RAM 从未被装载。
   修正为标准序列（Seq 04=06 sequential + GC 06=00 A0000 窗 + 写后恢复）。
2. **CRTC 写保护吞掉恢复写**：gfx_enter 写 `CRTC11=0x8E`（bit7=保护）
   后，text_enter 对 0x00-0x07 的恢复写**全部被硬件静默丢弃**，
   gfx 的垃圾垂直时序（06=0x0B：垂直总数 11 行！）残留 → 往返后花屏。
   text_enter 现在先 `0x11=0x0E` 解锁再恢复。
3. **gfx_enter 的 Mode 13h 垂直时序表本身是错的**（BIOS 标准表
   04=55 05=81 06=BF 07=1F 09=41），已改为标准值 —— 修复后 gfx 模式
   在 QEMU 里完整渲染：**中文 + 8 种样式全部正确显示**。
4. **vdiag 一跑屏幕永久黑**：text_diag 读 Attribute Controller 时
   索引写不带 0x20（Video Enable）位 —— VGA 协议里这会关掉视频输出
   且不恢复。已修（所有 AC 索引写 OR 0x20，收尾 0x20 恢复）。
5. **mkelf.py 的 e_entry 用了文件偏移**（0x1000，而段 vaddr=0），
   加载器入口校验拒绝 → -5。改为 0，并在自检里加了入口范围断言；
   随后发现手编机器码把 msg 指针编成绝对地址，PIE 滑动后指向页 0
   被 validate_uaddr 拒绝（exit 走得通、write 静默失败）—— 改为
   EIP 相对取址（call $+5 / pop）。最终 QEMU 实机输出：
   `hello from ELF` + `[task 1 'hello' exit(0)]` —— **ELF32 PIE+ASLR
   加载链路真机全通**。mmaptest 同样真机全绿（mmap/懒映射/munmap/
   空洞复用/非法拒绝，`MMAPTEST PASS`）。

配套修复：shell `run_from_path` 对带扩展名的命令不再强拼 `.ce`
（`hello.elf` 之前被拼成 `hello.elf.ce`）；format 确认是整行读取
（回归脚本补回车）；回归脚本 PNG 解析支持 32bpp screendump。

**已知遗留（纯外观）**：gfx→text 往返后文本屏有对角 RGB 细条纹。
排查结论：CRTC/Seq/GC/AC/DAC 寄存器经物理内存 dump 证明与干净 boot
逐字节一致、内容无关（clear 后仍在）、-vga cirrus 与 std 输出相同、
Seq 复位脉冲/AC 开关/平面清零均无效 —— 判定为 QEMU std-VGA 内部
surface 状态问题，真机硬件无 surface 缓存，预期不复现；后续可从
QEMU vga.c 源码级追查。诊断基建保留：`vga_reg_dump[]` 全局数组 +
monitor `xp` 直读物理内存，可随时对比任意两个时刻的完整 VGA 寄存器组。

摘标结论：v2.9.1 的 `[MTRR/pan UNTESTED]` 中，VBE 模式进入/退出、
text 同意门、闪烁光标、mmap、ELF 均已实机验证；MTRR WC 与
HW-pan 滚动本身仍需在 `vbe` 模式下跑 `vdiag` 确认（本轮未跑 VBE 路径）。
