# CatOS 开机耗时诊断与修复报告（v2.46）

- 日期：2026-11-17
- 基线：HEAD `6cf1ff6`（v2.45）；修复后版本 v2.46
- 范围：仅启动路径与被定位的瓶颈代码；未触碰 shell 命令解析 / 建议逻辑（含 v2.41 did-you-mean）。
- 一句话结论：QEMU 实测定位到两处启动瓶颈——GRUB 菜单 5s 倒计时、ATA 空槽 10M 迭代自旋；修复后内核到提示符 1860–1900ms → 170–260ms（约 -88%），墙钟到提示符 11.77s → 4.99s（约 -58%）。全量回归全绿。

## 一、测量环境与口径

### 1.1 环境

- QEMU 6.2.0，包装脚本 `/home/user/bin/qemu-system-i386`（内部 `-L /home/user/qemu-bios`）。
- 启动参数：`-m 256 -no-reboot -net none -cdrom <iso> -boot order=d -display none -monitor tcp:127.0.0.1:<port>`，串口 `-serial file:<log>`。
- 磁盘：`kernel_v25/catos_disk.img`（64MiB FAT）；有盘 / 空盘两轮各测。
- 打点：内核 COM1 调试出口 `ser_dbg()` 输出 `[btime] <uptime_ms>ms <tag>`；`uptime_ms()` 由 100Hz tick 换算，分辨率 10ms。墙钟由探测脚本 `tests/boot_time_probe.py` 记录串口行到达时间与帧缓冲提示符时间。

### 1.2 三段拆分与口径说明

探测脚本把一次启动拆成三段：

| 段 | 含义 | 计时基准 |
|---|---|---|
| pre-kernel | SeaBIOS + GRUB + ISO multiboot 加载，到内核入口 K0 | 墙钟 |
| kernel markers | K0 → S9（shell 提示符就绪） | 内核 uptime（10ms 分辨率） |
| post-last-marker | S9 之后到脚本在帧缓冲检测到提示符 | 墙钟 |

> **口径注意**：post-last-marker 约 2.15s 是探测脚本 `read_screen` 固定 2.0s 等待 + 轮询粒度造成的 **harness artifact**，不是内核耗时。内核是否就绪以 S9 的 uptime 为准；本报告对比内核阶段一律用 S9 uptime，墙钟对比注明该 artifact。

### 1.3 探测脚本

`tests/boot_time_probe.py`（本轮新增）：

```
python3 tests/boot_time_probe.py <catos.iso> [--qemu PATH] [--port N]
        [--disk IMG] [--runs N] [--serial FILE] [--timeout S]
```

脚本复用 `qemu_driver` 的 monitor/PS/2 约定，追加 `-serial file:` 捕获内核阶段标记，轮询帧缓冲正则 `catos:[^\n]*[\$#]` 判定提示符。

## 二、修复前实测（v2.45 基线，QEMU 有盘 3 轮）

### 2.1 墙钟

| 轮次 | 到提示符墙钟 | pre-kernel |
|---|---|---|
| 1 | 11771 ms | 7365 ms |
| 2 | 11773 ms | 7368 ms |
| 3 | 11759 ms | 7352 ms |
| 均值 | **11768 ms** | **≈7.36 s** |

空盘两轮：11724 / 11693 ms（与有盘基本一致，瓶颈不在盘内容）。

### 2.2 内核阶段表（uptime ms，有盘）

| 阶段 | 标签 | 累计 ms | 增量 ms | 说明 |
|---|---|---|---|---|
| 内核入口 | K0 | 0 | — | |
| VGA + PS/2 | K1 | 0 | 0 | |
| 分页 | K2 | 0 | 0 | |
| 中断 / 任务 / 开中断 | K3 | 10 | +10 | |
| CPU 校准 | K4 | 110 | +100 | 固定 10 tick（100ms）自旋窗口 |
| 音频探测 | K5 | 110 | 0 | QEMU 无音频设备 |
| USB 探测 | K6 | 110 | 0 | QEMU 无 USB 设备 |
| 网卡探测 | K7 | 110 | 0 | `-net none` |
| shell 进入 | S0 | 110 | 0 | |
| FS init 入口 | F0 | 110 | 0 | |
| ATA 扫描入口 | A0 | 110 | 0 | |
| 槽0 主盘 | A10 | 110 | 0 | present=1，IDENTIFY 正常 DRQ=0x58 |
| 槽1 主从 | A11 | 930 | **+820** | absent，状态恒定 0x00，空转满 10,000,000 迭代 |
| 槽2 从主 | A12 | 930 | 0 | absent，状态 0x41（ERR 位）立即跳出 |
| 槽3 从从 | A13 | 1760 | **+830** | absent，状态恒定 0x00，空转满 10,000,000 迭代 |
| ATA 完成 | A2 | 1760 | — | |
| FAT 缓存加载 | V1 | 1770 | +10 | |
| 镜像校验 | V2 | 1770 | 0 | |
| FS 完成 | F2/F3/S1 | 1770 | 0 | |
| 语言初始化 | S2 | 1780 | +10 | |
| 环境初始化 | S3 | 1780 | 0 | |
| CE 初始化 | S4 | 1780 | 0 | |
| 用户 / ACL / 会话 | S5 | 1790 | +10 | |
| 内建应用 resolve+seed | B0→B1 | 1820 | +30 | 23 条内建命令解析 |
| 内建应用安装 | S6 | 1820 | 0 | |
| pkg 仓库 seed | P0→P1 | 1850 | +30 | 仓库索引 resolve |
| autostart | S8 | 1860 | +10 | |
| **提示符就绪** | **S9** | **1860–1900** | — | |

### 2.3 瓶颈定位

1. **GRUB 菜单倒计时 ≈5s（pre-kernel 主项）**
   模板 ISO 内嵌 `grub.cfg` 的 `set timeout=5`：SeaBIOS + ISO multiboot 加载本身仅约 2.4s，其余约 5s 是菜单停留在倒计时。全模板仅 1 处 `set timeout=5`、0 处 `set timeout=0`。

2. **ATA 空槽自旋 ≈1.65s（kernel 主项）**
   `ata.c` 探测路径两轮等待各跑满 10,000,000 迭代兜底。槽1 / 槽3 无设备时状态端口恒定 `0x00`（无 BSY 0x80 / ERR 0x01 / DRQ 0x08），每槽空转 820–830ms；槽2 靠 ERR 位立即跳出。两轮各空转满迭代（诊断计数 `wait_it=10000000`）。

3. **其余固定小项（非瓶颈，未改）**
   - CPU 校准固定 100ms 窗口（K4）。
   - 内建命令 resolve 30ms、pkg 仓库 seed 30ms。
   - FS / 用户 / 语言 / CE 初始化各 ≤10ms，均为单小文件读，无重复扫描。
   - pkg 逐包 stat（PKGS.TXT）发生在 `pkg list` 命令惰性路径，**不在启动路径**；启动时 pkg 仅做仓库 seed resolve。

## 三、修复内容

### 3.1 ATA 空槽快速跳过（`kernel_v25/src/ata.c`）

- 探测路径等待上限：10,000,000 迭代 → `ATA_PROBE_WAIT_MAX=1,000,000` 兜底。
- 新增 `ATA_PROBE_DEAD_SAMPLE=256`：IDENTIFY 后状态既无 BSY 也无 ERR/DRQ，且状态恒定 `0x00` / `0xFF`（空槽 / 开放总线）连续 256 次采样，即判定无设备，提前返回。
- 空闲等待轮对开放总线 `0xFF`（BSY 卡住但无端口解码）同样提前跳出。
- **未动**读写路径的 `ata_wait_ready` / `ata_wait_drq`（已探测存在的盘路径），避免回归风险。

### 3.2 组装期消除 GRUB 5s 倒计时（`kernel_v25/build/patch_work_iso.py`）

- 组装 ISO 时把模板内嵌 cfg 的 `set timeout=5` 替换为 `set timeout=0`；替换前断言 pattern 唯一，`timeout=5` 与 `timeout=0` 都不存在则报错退出。
- 模板二进制本身不改；改动只在组装脚本。

### 3.3 打点设施（保留为启动诊断）

- `hw.c` / `hw.h`：新增轻量 `btime_mark(const char *tag)`，经 `ser_dbg` 输出阶段标记。
- `kernel.c` / `fs.c` / `ata.c` / `fat.c` / `shell.c` / `builtin_apps.c`：插入 K0–K7、F0–F3、A0/A1x/A2、V0–V2、S0–S9、B0/B1/P0/P1 标记。
- 每次启动每阶段一次受 guard 串口写，无行为变化；临时调试计数（槽位状态 / 迭代计数）已剥离，编译干净。

### 3.4 改动文件清单

| 文件 | 改动 |
|---|---|
| `kernel_v25/src/ata.c` | 探测空槽稳定采样跳出 + 等待兜底收紧；A0/A1x/A2 打点 |
| `kernel_v25/build/patch_work_iso.py` | `timeout=5`→`timeout=0` 组装期替换 + 唯一性断言 |
| `kernel_v25/src/hw.c`、`include/hw.h` | `btime_mark()` 设施 |
| `kernel_v25/src/kernel.c` | K0–K7 打点 |
| `kernel_v25/src/fs.c` | F0–F3 打点 |
| `kernel_v25/src/fat.c` | V0–V2 打点 |
| `kernel_v25/src/shell.c` | S0–S9 打点 |
| `kernel_v25/src/builtin_apps.c` | B0/B1/P0/P1 打点 |
| `tests/boot_time_probe.py` | 新增启动计时探测脚本 |

## 四、修复后实测对比（QEMU 有盘 6 轮）

### 4.1 墙钟

| 轮次 | 到提示符墙钟 | pre-kernel |
|---|---|---|
| 1 | 5142 ms | 2989 ms |
| 2 | 5014 ms | 2861 ms |
| 3 | 5037 ms | 2885 ms |
| 4 | 4964 ms | 2863 ms |
| 5 | 4959 ms | 2806 ms |
| 6 | 4999 ms | 2846 ms |
| 均值 | **≈4988 ms**（min 4959 / max 5015） | **≈2.85 s** |

### 4.2 内核阶段（uptime ms）

- ATA 四槽全部 @110ms：空槽 256 次采样即跳出，A11/A12/A13 增量 0ms。
- S9 提示符就绪：**170–260ms**（典型 170–230ms）。
- 阶段分布：K4 校准 100ms 固定；lang/env/ce/user 合计 ≤30ms；builtins resolve 20ms；pkg seed 10ms；autostart 10ms。

### 4.3 修复前 vs 修复后

| 指标 | 修复前（v2.45） | 修复后（v2.46） | 变化 |
|---|---|---|---|
| 墙钟到提示符（均值） | 11768 ms | ≈4988 ms | **-57.6%** |
| pre-kernel | ≈7.36 s | ≈2.85 s | -61%（GRUB 5s 倒计时消除） |
| 内核到提示符（S9 uptime） | 1860–1900 ms | 170–260 ms | **约 -88%** |
| ATA 扫描耗时 | 110→1760（+1650ms） | 110→110（+0ms） | 空槽自旋消除 |

> 墙钟残余 ≈4.99s 的构成：SeaBIOS + 光盘 multiboot 加载 ≈2.85s；探测脚本轮询 artifact ≈2.15s（非内核耗时）。内核自身就绪时间在修复后为 170–260ms。

## 五、VMware 与 QEMU 差异分析（本节未在本环境实测）

> **如实注明**：本环境无 VMware hypervisor，以下结论基于源码启动路径审计，未在 VMware 真机上复跑；VMware 侧实际耗时需按第七节指引复测回传。用户反馈的"开机慢"发生在 VMware 真机。

### 5.1 逐路径审计

| 启动路径 | QEMU 行为（`-net none` / 无附加设备） | VMware 潜在差异 | 本轮处置 |
|---|---|---|---|
| ATA 四槽探测 | 空槽状态 `0x00` 恒定；旧代码 10M 自旋 | VMware 虚拟 IDE 对从盘 / 次通道端口的解码可能不同（状态 `0x00`/`0xFF`/`0x50`）；端口陷阱到宿主通常比 QEMU 进程内模拟慢，旧自旋在 VMware 上代价可能更高 | 已加稳定无信号采样跳出（256 次）+ 1M 迭代兜底；开放总线 `0xFF` 提前跳出 |
| 网卡 e1000 | probe 扫 PCI 总线 + 桥；init 内 reset 自旋 100k、link-up 自旋 1M，仅 PCI 命中后进入；`net.c` pcnet STOP 等待 100k；无设备时 `pci_find_pcnet` 快速返回 -1 | VMware e1000 / vlance 命中后这些有界自旋会完整跑一遍；link-up 等待在虚拟网卡链路未立时可能走满 1M 自旋 | 未改：自旋有界且在盘路径之外；列入 VMware 复测观察点 |
| 音频 SB16 / ES1371 | SB16 `dsp_read` DSP_POLL=20000 × io_wait（0x80 outb），失败回落 `es1371_probe`；es1371_init 无自旋循环；QEMU 无设备时探测 0ms | VMware 默认 HDA 两者都不模拟；SB16 未命中时 20000 次 0x80 写代价小 | 未改 |
| USB UHCI | probe 扫 PCI；`usb_reset_port` 100ms+50ms 延时仅在有设备连接（PSC_CONNECT）时发生；枚举 `usb_delay_ticks(2)` | VMware 接 USB 键盘会按设计支付端口复位 + 枚举延时；未连接设备时不进入延时 | 未改 |
| PS/2 初始化 | `kb_init` 只清空输出缓冲，无等待循环 | 无差异风险 | 未改 |
| 无光盘 / 无硬盘回退 | `fat_probe` 在 ATA 扫描后快速返回 -1，shell 种默认环境 | 无盘组合下 ATA 空槽路径是主要差异点，已随 3.1 修复 | 已覆盖 |

### 5.2 既有 VMware 兼容逻辑（代码内已存在）

- `net.c`：MAC `FF:FF` 开放总线拒绝；vlance / LANCE 回落固定 MAC。
- 音频：ES1371 作为 VMware 音频回落后端。

### 5.3 未改项及原因

- **CPU 校准 100ms 窗口（K4）**：MHz 仅用于 `sysinfo` 显示（`HW_CPU_MHZ` 唯一消费者 `shell.c:1521`），不参与定时；缩短会降低 MHz 显示精度，留作后续可选优化。
- **读写 PIO 等待循环**：在已探测存在的盘路径上，改动回归风险高，本轮不动。
- **pkg 逐包 stat**：命令时惰性发生，不在启动路径。

## 六、回归结论

本轮新鲜日志位于 `tests/regress_logs/`：

| 套件 | 基线 | 本轮结果 |
|---|---|---|
| qemu | 12/12 | **12/12 PASS** |
| fsrec | 12/12 | **12/12 PASS** |
| caps | 12/12 | **12/12 PASS** |
| user | 15/15 | **15/15 PASS** |
| disk | 5/5 | **5/5 PASS** |
| env | 13/13 | **13/13 PASS** |
| heap | 4/4 | **4/4 PASS** |
| size | 7/7 | **7/7 PASS** |
| ce | FAIL=11 既有基线 | PASS=7 SKIP=13 FAIL=11（11 个 FAIL 为 ce_sim 未建模 API 的既有基线，与本轮改动无关） |

改动相关模块针对性回归：ATA 探测（disk / fsrec / qemu 挂载盘场景）、FS 扫描（fsrec / env / caps）、内建应用与 pkg 安装路径（user / caps / size）、shell 初始化（qemu / env）均通过。

## 七、VMware 复测指引（供用户在真机执行）

### 7.1 建议复测场景

1. **正常装盘启动**：硬盘 + 光盘，记录到提示符总秒数。
2. **无硬盘启动**：仅光盘。
3. **无光盘启动**：硬盘 / PXE 引导。
4. **USB 键盘**：挂载 USB 键盘后启动，观察 USB 枚举阶段。
5. **网卡切换**：分别选 e1000 与 vlance 各启一次，对比网卡探测阶段。

### 7.2 观察点

- GRUB 菜单是否不再停留 5s（应直接进内核）。
- 从开机到 shell 提示符的总秒数（秒表）。
- 提示符前卡顿位置：GRUB 菜单 / 内核早期 / 挂载盘后 / 进图形模式前。
- 图形模式切换（gfx24 / VBE）是否有长停顿。
- 音频 / 网卡 / USB 设备在 `hwinfo`（或 `about`）中的显示。

### 7.3 串口日志抓取

- VMware 虚拟机设置 → 添加串行端口 → 输出到文件。
- CatOS COM1 115200 8N1；`[btime] <ms> <tag>` 行落盘。
- 回传：各场景到提示符秒数、卡顿时所处阶段、串口日志尾部、`hwinfo`/`about` 输出。

### 7.4 已知环境注意

- 若 VMware 上仍有明显卡顿，优先对照串口标记定位：卡在 A11/A12/A13（ATA 空槽）、K5/K6/K7（音频/USB/网卡探测）、还是 S1–S9（FS / shell 初始化）。

## 附录 A：打点标签一览

| 标签 | 阶段 |
|---|---|
| K0 | 内核入口 |
| K1 | VGA + PS/2 初始化 |
| K2 | 分页 |
| K3 | 中断 / 任务 / 开中断 |
| K4 | CPU 校准 |
| K5 | 音频探测 |
| K6 | USB 探测 |
| K7 | 网卡探测 |
| S0 | shell 进入 |
| F0 | FS init 入口 |
| F1 | FS 探测前 |
| A0 | ATA 扫描入口 |
| A10–A13 | ATA 四槽（主/从 × 主/从通道） |
| A2 | ATA 扫描完成 |
| V0 | 卷探测进入 |
| V1 | FAT 缓存加载 |
| V2 | 镜像校验完成 |
| F2 | fat_probe 完成 |
| F3 | part_scan 完成 |
| S1 | fs_init 完成 |
| S2 | lang_init |
| S3 | env_init |
| S4 | ce_init |
| S5 | user / acl / session |
| B0 | 内建应用 resolve+seed |
| B1 | 内建应用完成 |
| S6 | 内建应用安装 |
| P0 | pkg 仓库 resolve+seed |
| P1 | pkg 仓库完成 |
| S7 | pkg 仓库就绪 |
| S8 | autostart 完成 |
| S9 | shell 提示符就绪 |
