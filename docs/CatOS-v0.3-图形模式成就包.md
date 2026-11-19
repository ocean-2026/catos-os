# CatOS v0.3 图形模式（Mode 13h）成就包

> **归档说明（2026-11-09 仓库治理）**
> 本文档原为仓库根目录的 `README.md`，随根目录治理迁入 `docs/`。
> 下方「二、文件清单」列出的是 **2026-11-05 成就包的历史快照**，其中
> `gfx.h` / `gfx.c` / `vga.c` / `shell.c` / `shot-gfx-mode.png` /
> `shot-text-restored.png` 为当时的一次性快照文件，**已于 2026-11-09 从仓库中删除**
> （零引用核实 + 单一源码事实为 `kernel_v25/`），`build.sh` 保留为委托 shim。
> 本文档内容作为**历史记录**保留，描述的 QEMU VGA 字形 bug 根因分析依然有效。

**日期**: 2026-11-05
**状态**: 已完成并在 QEMU 实测通过

## 一、本包包含的新成就

### 1. 内核图形模式（VGA Mode 13h）
- **320x200x256** 帧缓冲模式，8x8 点阵软终端（40x25 网格）
- shell 新增命令：**`gfx`** 进入图形模式、**`text`** 返回文本模式
- DAC 调色板：0-15 标准 VGA 色 + 16-255 灰度渐变
- 图元：putpixel / fillrect / hline / vline / rect / drawchar / text
- 图形演示：渐变横带 + 白边框 + 标题 + 16 色块 + 信息文本

### 2. 关键 bug 修复：`text` 切回后竖条纹屏（字形缺失）
**根因**（QEMU v8.2.0 `hw/display/vga.c` 源码取证）：
- QEMU 的 VGA 内存窗口映射由 `GC 0x06` 决定：`memory_map_mode = (GR06>>2)&3`
- 文本模式 `GR06=0x0E` → **mode 3（B8000 32KB 窗口）**：
  ```c
  case 3: addr -= 0x18000; if (addr >= 0x8000) return;  /* 静默丢弃 */
  ```
- CPU 向 **0xA0000** 的写入被判定为窗口外地址（addr 下溢），**全部静默丢弃**
- 因此旧版 `vga_reload_font()` 写 0xA0000 字形区完全无效，QEMU 渲染引擎读到 chain4 污染后的垃圾 → 竖条纹
- 图形模式 `GR06=0x05` → mode 1（A0000 64KB 窗口），0xA0000 写有效，故 gfx 正常

**修复**：`vga_reload_font()` 改经 **0xB8000 窗口** + **latched 写**（`GR05=0x00`）：
- 窗口偏移 `a = 32*c + r` → `vram[4*a + plane] = vram[128*c + 4*r + 2]`
- 与 QEMU 渲染读取位置（`font_base = vram + 2 + 128*ch`）**逐字节一致**

**验证**：
- 内核 `vdiag` 命令读回 `'A'` 字模 = `00 00 20 20 50 50 88 88`，与内置 font8x8_basic 完全一致
- QEMU 实测：gfx 图形模式渲染正常 → text 恢复文本正常（无竖条纹）→ 再次 gfx → text 多轮往返稳定

## 二、文件清单

| 文件 | 说明 |
|---|---|
| `gfx.h` | 图形模式头文件（API + 常量） |
| `gfx.c` | Mode13h/Text03 寄存器序列、DAC、图元、8x8 软终端、字形重载 |
| `vga.c` | vga_clear/putc/set_color/set_fg 图形分支 + `vdiag` 诊断命令 |
| `shell.c` | 命令分发表新增 `gfx` / `text` / `vdiag` |
| `build.sh` | 编译链接列表加入 gfx.o |
| `catos.iso` | 完整可启动镜像（含图形模式 + 全部修复） |
| `shot-gfx-mode.png` | QEMU 实测：图形模式正常渲染 |
| `shot-text-restored.png` | QEMU 实测：text 恢复后文本正常（无条纹） |

## 三、QEMU 验证命令
```bash
qemu-system-i386 -m 128 -cdrom catos.iso -boot order=d
# shell 内: gfx 进入图形模式; text 返回文本模式; vdiag 查看 VGA 寄存器与字形校验
```
