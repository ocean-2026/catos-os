# CatOS 通用操作系统执行计划

> **状态**：已批准的战略计划（2026-11-16）
> **战略选择**：自有 ABI + 自长生态；CE 旧世界作为兼容人格保留，永不破坏。
> **核心原则**：**换内部，不换外部**——通用化是内核内部革命，用户看到的 shell、命令、
> 图形模式、使用流程大多不变。图形模式的身份就是"shell 的图形后端"，不是独立 GUI。
> **取代关系**：本文取代 `docs/ROADMAP-通用操作系统.md` 中"重写级、当前不做"的判定
> （独立地址空间 / SMP / 动态内存三项由"不做"转为 P0–P1 必做）。旧文保留为历史决策记录。
> **问题级明细**：`docs/CatOS-内核问题清单.md`；**每轮进度**：`docs/PROGRESS.md`。

---

## 0. 不可动摇的约束（任何阶段都不得违反）

| # | 不变量 | 理由 |
|---|---|---|
| I1 | `catos_api_t` / syscall enum **append-only**，只在尾部追加 | 老 `.ce` 按偏移索引函数指针表，插字段即错位崩溃 |
| I2 | 每阶段收尾必须通过 QEMU 实机回归 `tests/qemu_regress.py` **12/12**，以及 4GB / btop / dl / bg 四个探针 | 这是项目唯一的质量锚点，失去它等于失去可验证性 |
| I3 | 安全修复 A10（`be_vbe24.c` 的 `int64_t clip()`）、H1（ring3 不可 kill ring0）、H8（`/bin/*.ce` 只读封印）、H9（autoexec 只列出不执行）必须持续保留 | 隔夜包曾回退这四项，合并时已重新施加 |
| I4 | **外部表现大多不变**：shell 命令集、CE 程序用法、gfx/text 切换、交互流程保持原样；native 能力是**叠加**不是替换 | 用户明确要求；通用化是内部革命 |
| I5 | **图形模式 = shell 的图形后端**：是"更自由的文本模式"，服务命令行，不是 GUI / 窗口系统 | 见第 5 节；这条决定显示子系统的全部演进边界 |
| I6 | 构建前 `export PATH=/c/msys64/mingw32/bin:$PATH` | 脚本内 `/d/Tools/MSY32/...` fallback 已损坏 |
| I7 | 每个阶段都要有可运行产物，禁止"大爆炸式"重写后一次性合并 | 双人格并存，旧树始终可启动 |

---

## 1. 目标态：什么叫"通用 OS"达成

分层定义，全部满足才算达成；每项都给出可检验判据。

| 层 | 目标态 | 判据 |
|---|---|---|
| **外部表现** | **与当前基本一致，用户零迁移** | shell 命令、CE 程序、gfx/text、btop/后台任务照常工作；新能力为增量 |
| 硬件 | 64 位 x86-64，4 核 SMP，识别 4GB+ 内存 | QEMU `-m 3840 -smp 4` 下 shell 正常，4 核均有负载 |
| 内核 | 独立地址空间 + COW + swap + 抢占式多核调度 | `fork` 后父子内存隔离；多线程任务跨核运行 |
| 存储 | VFS 抽象 + ext2/ext4 + FAT32 并存 | `mount -t ext2` 挂真实镜像，跨文件系统路径统一 |
| ABI | native 数字 syscall（版本化）+ legacy CE 人格并存 | 同一内核同时运行 `hello.ce` 与 ELF64 `hello` |
| 用户态 | 自有 libc + 用户态动态链接器 + 交叉工具链 | 从源码编译并运行一个真实 C 程序 |
| 网络 | socket API + 完整 TCP | 能向外部 HTTP 服务发起 GET 并读回响应 |
| 安全 | POSIX 凭据 + 能力位 + NX/SMEP/SMAP + KASLR | 普通用户无法读内核内存、无法 kill 内核任务 |
| 生态 | 包管理依赖解析 + 自举环 | 系统内编译系统自身（阶段性） |

**明确非目标**：X11/Wayland 兼容、桌面环境、GPU 加速、Linux 二进制兼容、多用户远程登录。
**明确不变**：shell 提示符、命令拼写与参数、`gfx`/`text` 切换流程、CE 应用运行方式、后台任务语法。

---

## 2. 双人格架构（总设计）

```
用户看到的东西（全程不变）
  shell · 命令集 · CE 程序 · gfx/text 终端 · btop/后台任务
                       │
        ┌──────────────┴──────────────┐
        │                             │
  legacy personality            native personality
  CE01 / CSO1                   ELF64 / .so
  int 0x80 thunk                syscall + vDSO
  catos_api_t（冻结）            数字 syscall 表（版本化）
        │                             │
        └──────────────┬──────────────┘
                       │
                统一内核核心
     VMA / 调度 / VFS / 网络 / 驱动 / 显示后端
```

- **用户视角恒定**：无论内部跑哪套人格，用户始终用同一个 shell、同一套命令。native 人格
  只是让系统"能跑更多程序"，**不改变用户与系统的交互方式**。
- **legacy personality**：现有全部代码原样保留，跑在 IA32 兼容模式，`int 0x80` 入口不变。
- **native personality**：新建，long mode，数字 syscall，自有 libc（POSIX 形状但不承诺二进制兼容）。
- 两者共用内核核心，但**不共用 ABI**；禁止把 `catos_api_t` 改造成通用 ABI（I1）。

---

## 3. 四阶段执行计划

> 阶段安排已按"外部表现尽早恢复"调整：P0 是纯内部地基（允许只有串口输出），
> **P1 收尾就把 shell 与显示后端移植到 64 位内核**，让用户可见外观回到与 v25 一致，
> 之后的 P2/P3 都是在这个"外观不变的壳"里继续换内部。

### P0 — 64 位地基（预计 2–4 周）

**目标**：证明 64 位内核能独立启动并运行一个 ELF64 用户程序；此阶段完全不接 CE、不接图形后端（过渡态）。

| # | 任务 | 交付 | 状态 |
|---|---|---|---|
| P0.1 | 新建 `kernel64/` 平行树，GRUB 引导进 long mode | `kernel64/` 骨架 | ☐ |
| P0.2 | PML4 四级页表 + higher-half 内核映射 | `paging64.c` | ☐ |
| P0.3 | 移植 PMM 到 64 位；解析 multiboot mmap，支持 4GB+ | `pmm64.c` | ☐ |
| P0.4 | long mode IDT + 异常处理；COM1 串口 early printk | `idt64.c`, `serial.c` | ☐ |
| P0.5 | syscall MSR 初始化（STAR/LSTAR/CSTAR, EFER.SCE） | `syscall64.c` | ☐ |
| P0.6 | 最小 ELF64 加载器（PT_LOAD），跑通 ring3 `hello` | `elf64.c` + demo | ☐ |

**验收**：`qemu-system-x86_64 -m 3840 -smp 4 -cdrom kernel64/catos64.iso` → 串口输出 shell prompt；
ELF64 `hello` 打印成功；旧树 `kernel_v25` 回归仍 12/12。

**回退**：`kernel64/` 完全独立，出问题直接回到 `kernel_v25`，零污染。

---

### P1 — 内核核心通用化 + 外观回归（预计 4–8 周）

**目标**：补齐通用 OS 的内核能力；这是旧路线图判定"不做"的三项转正的地方。
**阶段结束时，64 位内核的用户可见行为必须与 v25 一致**（shell、gfx、CE 程序照常）。

| # | 任务 | 交付 | 状态 |
|---|---|---|---|
| P1.1 | VMA 线性链表 → 红黑树 | `vma.c` 重构 | ☐ |
| P1.2 | `fork()` COW；页错误路径处理写时复制 | `fork64.c`, `pf64.c` | ☐ |
| P1.3 | slab 分配器 + per-CPU 缓存 | `slab.c` | ☐ |
| P1.4 | swap 页（镜像后端），页面回收 | `swap.c` | ☐ |
| P1.5 | APIC 定时器 + 每 CPU 运行队列 + CFS 类调度 | `smp.c`, `sched64.c` | ☐ |
| P1.6 | 内核同步原语：自旋锁、互斥锁、RCU | `sync.c` | ☐ |
| P1.7 | 动态任务表，突破 `MAX_TASKS 16`；线程组 | `task64.c` | ☐ |
| P1.8 | VFS 抽象（superblock/inode/dentry/file）+ ext2 驱动 | `vfs.c`, `ext2.c` | ☐ |
| P1.9 | **外观回归**：把 shell 命令集、`be_text`/`be_gfx` 后端、CE 人格移植到 64 位内核 | `shell64.c`, `compat32/`, 后端移植 | ☐ |

**验收**：`-smp 4` 下 4 个 CPU 都有执行时间；`fork` 后父子写同一页互不影响；
`mount -t ext2` 挂上真实 ext2 镜像并读写文件；
**用户可见行为与 v25 一致：shell 命令、`gfx`/`text` 切换、CE 程序、btop、后台任务全部照常**；
I2 全部探针 PASS。

**风险**：打开内核抢占会动摇依赖非抢占的 static 缓冲安全（旧路线图第 14.2 条）。
**前置**：先把 `copy_from_user()` 快照化扩展到所有 syscall 路径，再开抢占。

---

### P2 — 用户态与 ABI（预计 4–8 周）

**目标**：native ABI 成型，双人格在同一内核上共存；用户看不到变化，只是多了能跑的程序类型。

| # | 任务 | 交付 | 状态 |
|---|---|---|---|
| P2.1 | 数字 syscall 表（带版本号）+ syscall 分发 | `sys64.c` | ☐ |
| P2.2 | vDSO（gettimeofday 等免陷入调用） | `vdso.S` | ☐ |
| P2.3 | ELF64 加载器完整化：PIE/ASLR、`.interp`、`R_X86_64_*` 重定位 | `elf64.c` | ☐ |
| P2.4 | 用户态动态链接器 `ld-catos.so`：GOT/PLT 懒绑定、符号版本 | `ld-catos/` | ☐ |
| P2.5 | `.so` 物理页共享（多进程 dlopen 同一库只读一份） | 内核 mmap 文件映射 | ☐ |
| P2.6 | 自有轻量 libc `libcatos`（POSIX 形状，自有 ABI） | `libcatos/` | ☐ |
| P2.7 | 交叉工具链 `x86_64-catos-gcc` 与构建脚本 | `toolchain/` | ☐ |
| P2.8 | legacy CE 人格在 64 位内核上完整回归（IA32 兼容模式） | `compat32/` | ☐ |

**验收**：同一内核同时跑 `hello.ce` 与 ELF64 `hello`；两个进程 `dlopen` 同一 `.so` 只占一份物理页；
native syscall 延迟低于 `int 0x80`；**外部表现仍与 v25 一致**；I1/I2 不变。

**说明**：CSO1 不删除，降级为 legacy 人格的轻量快路径；native 侧用标准 ELF `.so`。

---

### P3 — 生态、驱动与安全（预计 8 周以上，持续演进）

**目标**：在外观不变的前提下，补齐生态与安全性。

| # | 任务 | 交付 | 状态 |
|---|---|---|---|
| P3.1 | PCI/PCIe 枚举 + 驱动注册框架 | `pci.c`, `drv.c` | ☐ |
| P3.2 | e1000 / virtio-net 驱动 | `netdrv.c` | ☐ |
| P3.3 | socket API + TCP 状态机（重传/拥塞控制） | `socket.c`, `tcp.c` | ☐ |
| P3.4 | AHCI 驱动；多磁盘 | `ahci.c` | ☐ |
| P3.5 | POSIX 凭据（uid/gid/组）+ capabilities + DAC | `cred.c` | ☐ |
| P3.6 | NX / SMEP / SMAP / KASLR | `sec64.c` | ☐ |
| P3.7 | seccomp 式系统调用过滤 | `seccomp.c` | ☐ |
| P3.8 | 包管理器依赖解析；自举环（系统内编译系统自身） | `pkg`, `cc_self` | ☐ |
| P3.9 | 显示后端按第 5 节增强（**仍是 shell 的图形后端，交互方式不变**） | `be_gfx` 增强 | ☐ |
| P3.10 | CI：全部探针自动跑 + kselftest + syscall fuzz + 静态分析 | `.ci/` | ☐ |

**验收**：外部 HTTP GET 成功；普通用户无法读内核内存或 kill ring0 任务；自举环闭环；
全部探针 CI 自动通过；**用户使用方式无需重新学习**。

---

## 4. 缺陷 → 方案总表（固化上一轮决策）

| 缺陷 | 方案 | 阶段 |
|---|---|---|
| 32 位 i386、用户区仅 64MB | long mode + PML4 + higher-half；虚拟地址空间扩到 128TB 级 | P0 |
| CE01 固定基址、扁平 ABI | 新增 ELF64 加载器 + PIE/ASLR；CE ABI 冻结为 legacy | P0/P2 |
| CSO1 无重定位、每任务复制页 | 用户态 `ld-catos.so` + GOT/PLT + `RELATIVE` 重定位 + 物理页共享 | P2 |
| 手写 thunk / 函数指针表 | 数字 syscall 表 + vDSO；`int 0x80` 仅服务 legacy | P2 |
| VMA 链表、无 COW/swap | 红黑树 VMA、COW、swap、slab | P1 |
| `MAX_TASKS 16`、无线程 | 动态任务表 + `clone()` 线程组 | P1 |
| 单核轮转、ring0 不可抢占 | APIC + 每 CPU 运行队列 + 锁/RCU + 内核抢占 | P1 |
| 自建 FAT32、无 VFS | VFS + ext2/ext4 + FAT32 并存 | P1 |
| 极简 TCP/IP | socket API + 完整 TCP + 多网卡驱动 | P3 |
| 驱动即轮询、无设备模型 | PCI 枚举 + 驱动框架 + ACPI 最小子集 | P3 |
| uid 0/100、ACL 简化 | POSIX 凭据 + capabilities + DAC + NX/SMEP/SMAP/KASLR | P3 |
| 无 libc / 生态 | 自有 `libcatos` + 交叉工具链 + 依赖解析 + 自举环 | P2/P3 |
| 可靠性靠人工 QEMU 脚本 | CI 自动回归 + kselftest + fuzz + 静态分析 | P3 |

---

## 5. 显示子系统：图形模式的定位与边界

> **最容易被误解的一节。用户已明确：图形模式只是 shell 的图形后端，
> 是"更自由的文本模式"，主要为命令行。它不是一个独立的图形环境，也没有自己的应用体系。**

### 5.1 现状（准确描述）

- 前端 `be_text` / 后端 `be_gfx`，`render.c` 开机默认切 `be_gfx`（Mode 13h / VBE，640×400 级分辨率）。
- `be_gfx` 本质是**彩色文字终端**：字形点阵渲染 + 前景/背景色 + 软终端网格（窄屏 40×12 自适应）。
- 已具备：`gfx_info` / `gfx_rect` / `gfx_blit`（syscall 61/62/63）、`gotoxy`（65）、像素级图元。
- `text` 命令保留物理按键 Y 确认才切回文本——这是安全护栏，不是 GUI 行为。
- btop 已证明：同一套命令行程序在 gfx 后端下自适应渲染，无换行/残影。
- **用户看到的图形模式里只有 shell**——没有窗口、没有桌面、没有独立 GUI 程序，
  这就是它应有的样子，通用化后依然如此。

### 5.2 演进方向（全部保持命令行语义）

| # | 增强 | 目的 |
|---|---|---|
| D1 | VBE LFB 高分辨率 + 可缩放字模 | 字符更多、更清晰；API 不变 |
| D2 | 自由像素坐标 + 文本层 z-order（`gotoxy` 扩展） | 比 80×25 更自由的排版 |
| D3 | 双缓冲 / 翻页 | 消除滚动与动画闪烁 |
| D4 | UTF-8 / 16×16 中文点阵 | 终端输出一致化 |
| D5 | TUI 构件库（面板/菜单/边框，纯字符语义） | 让命令行程序写出更好的界面 |
| D6 | 鼠标指针（可选，仅用于文本选择/点击） | 不引入窗口系统 |

### 5.3 明确不做

- ❌ 窗口管理器、合成器、桌面隐喻、多窗口叠加
- ❌ X11 / Wayland 兼容层、GPU 驱动
- ❌ 给图形模式做一套"独立应用体系"——它承载的应用就是 shell 与 CE/native 命令行程序
- ❌ 把 `gfx_*` 变成"应用画窗口"的 API——它是"应用画得更自由的字符"的 API

**验收**：所有 shell 命令与 TUI 程序在 gfx 后端下行为与 text 后端一致；btop 在 40×12 与高分辨率
下均无 wrap；不新增任何窗口/合成概念；图形模式下用户看到的仍然只有 shell。

---

## 6. 工程纪律

1. **每阶段收尾必须实机验证**，禁止以"编译通过"作为完成标准（I2）。
2. **双人格永不同时破坏**：任何改动后，legacy 回归 12/12 + 四探针必须全绿。
3. **ABI 变更走追加**：native syscall 表也采用版本号 + append-only，避免重演 CE 的兼容债。
4. **安全修复清单随阶段携带**（I3），每次合并外部代码都要逐项核对。
5. **外部表现回归是硬指标**（I4/I5）：每个阶段都要对比 shell 命令输出与图形界面，
   不允许"内部升级了但用户得重新学"的情况。
6. **文档滚动更新**：完成一批后，更新本计划对应行的状态（☐→☑）、`PROGRESS.md`，并追加当日 `.workbuddy/memory/` 日志。
7. **构建环境固定**（I6），跨机器先验证工具链。

---

## 7. 当前状态看板

| 阶段 | 状态 | 说明 |
|---|---|---|
| 前置：v2.47 合并 + 四项能力 + 安全修复 | ✅ 完成 | 2026-11-16，QEMU 全绿 |
| v2.48 TUI 实验室 + task_get ABI | ✅ 完成 | 2026-11-17：win-demo 六个 TUI 移植为 ring3 CE（ctop/cfm/charness/ctheme/cmenu/cgreet），新增 shell `tui` 命令；槽 69 task_get + HW_CPU_BUSY_PCT append-only；crash_attack 20/20（含新 A20 taskstat-abuse）；第二轮对抗另修 3 个蓝队洞（heap freelist C1/C3、SYS_WAIT 互锁 C3），详见 `PROGRESS.md` 第二十六节 |
| P0 64 位地基 | 🔶 准备中 | `kernel64/` 树已开、工具链已装（LLD + x86_64-elf binutils）；P0.1–P0.6 未验证，下一步从这里启动 |
| P1 内核核心 + 外观回归 | ☐ 未开始 | 此阶段结束恢复用户可见外观 |
| P2 用户态与 ABI | ☐ 未开始 | 外部无感 |
| P3 生态驱动安全 | ☐ 未开始 | 外部无感 |

---

## 8. 决策记录（ADR 摘要）

| 决策 | 选择 | 放弃的替代 | 日期 |
|---|---|---|---|
| 通用化路线 | 自有 ABI + 自长生态 | Linux 兼容层（成本高 10×，淹没现有代码） | 2026-11-16 |
| 变更范围 | **换内部，不换外部** | 连 shell/图形表现一起重做 | 2026-11-16 |
| 旧 CE 生态 | 冻结为 legacy personality | 迁移/重写全部 CE 程序 | 2026-11-16 |
| 图形模式 | shell 的图形后端，命令行优先 | 窗口系统 / GUI / 独立图形应用体系 | 2026-11-16 |
| 用户态 libc | 自有 `libcatos`（POSIX 形状） | 直接采用 musl 二进制兼容 | 2026-11-16 |
| 推进方式 | 四阶段增量，双人格并存 | 一次性大爆炸重写 | 2026-11-16 |

---

*本文件为执行纲领，随每批交付滚动更新。任何与旧 `ROADMAP-通用操作系统.md` 冲突之处，以本文为准。*
