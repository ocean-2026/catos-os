# CatOS 渲染层「前后端分离」设计说明

**分支**: `feat/render-split`（工作副本 `D:/CatOs-render-split`）
**基线提交**: `35de6c8`（2026-11-06 从共享工作树做的快照，含另一位工作者未提交的 v2.6 VMA/brk 改动与 2026-11-05 图形成果包）
**原目录 `D:/CatOs` 保持不动**，仍是 `master` + 原来的未提交改动。
**状态**: 内核与 ISO 均编译通过；本机无 QEMU，**未做实机启动验证**。

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
