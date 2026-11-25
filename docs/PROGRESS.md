# CatOS 进度汇报（2026-11-11）

> 覆盖窗口：2026-11-07 合流之后至 2026-11-11（含 v2.12~v2.14）· 主线 `D:/CatOs` master
> 上一份汇报（2026-10-30 自举/JS 解释器轮）随 git 历史保留。
> 行动纲领见 `docs/ROADMAP-通用操作系统.md`；问题明细见 `docs/CatOS-内核问题清单.md`。

---

## 一、总览

| 阶段 | 状态 | 提交 |
|---|---|---|
| 现项目 × 原项目合并取精华（30+ 提交）+ master 合流 | ✅ 完成 | `6945860` |
| SB16 声卡驱动 + 合流遗留修复 | ✅ 完成 | `cc631c8` |
| 用户指针跨页校验 P0（#78） | ✅ 完成 | `a5da8e3` |
| 800x600x24 真彩后端 + 用户绘图 ABI（#83~#85） | ✅ 完成 | `15cf106`（v2.12） |
| 消除劣根性第一批：syscall 边界/诊断/fsck/单一来源（#86~#89） | ✅ 完成 | `62cd529` |
| 应用与内核解耦（#90） | ✅ 完成 | `4fb3318` |
| 多用户 + 能力权限 + 初始 root（#91） | ✅ 完成 | `febc9b8`（v2.13） |
| ot / adofai 老项目收编 + pollkey + shell 文件四件套（#92~#95） | ✅ 完成 | `443c067`（v2.14） |
| GitHub 推送 | ❌ 未做 | 全部提交仅本地 |

---

## 二、本轮（11-10 ~ 11-11）完成明细

### 2.1 #86~#89 可靠性第一批（`62cd529`）
- **#86**：SYS_WRITE / SYS_FILE_WRITE / SYS_PIPE_WRITE 三处"校验后裸解引用"全部改为
  `copy_from_user()` 内核快照（分段/分块/一次性），`vprobe` 回归扩到 10/10。
- **#87**：新增 `diag.c`——致命故障记录向量/ring/任务/eip/CR2，shell `faultinfo` 可事后归因，
  `panic()` 统一停机入口；`diag_regress.py` 4/4。
- **#88**：`fat_verify_mirror()` mount 时校验并修复 FAT#2，shell `fsck` 手动重跑。
- **#89**：`build/apps.list` 成为应用清单单一来源，`test_app_list.py` 守卫。

### 2.2 #90 应用与内核解耦（`4fb3318`）
- `build/mkdisk.py` 宿主侧逐字节复刻 `fat_mkfs()` 布局，生成预装 20 应用的
  `catos_disk.img`；`build.sh disk` 一键产出。**磁盘成为应用真实来源，kernel.bin 只剩
  bootstrap fallback 语义**——改一个应用不再需要重编内核。
- 免 `format` 直接挂载；`disk_regress.py` 5/5。

### 2.3 #91 多用户与细粒度权限（`febc9b8`，v2.13）
- `src/user.c`：持久化 `/etc/passwd`（name:uid:gid:home:shell:caps:passhash）与
  `/etc/acl`（path:uid:gid:mode，12 位 Unix 语义）；种子 `root`（uid 0，CAP_ALL，
  **初始用户**）+ `user`（uid 100）。
- **能力位** CAP_NET/KILL/FSADMIN/AUDIO/USERADM/DEVICE/SYS 在**内核 syscall 分发处**
  强制（网络 25-32、音频 57-60 槽缺位返回 -13）；`task_kill` 改 root|属主|CAP_KILL。
- shell 新命令：whoami/id/lsuser/caps/login/su/useradd/userdel/passwd/chcaps/chmod/chown，
  提示符 `user@catos:/$`；`format`/`fsck` 要 CAP_FSADMIN，`sudo`/用户管理要 CAP_USERADM。
- **零新增 syscall 槽**，64 槽 ABI 不动。`user_regress.py` 15/15（端口 4465）。

### 2.4 #92~#95 ot / adofai 老项目收编（`443c067`，v2.14）
- 下载目录的 `catos-ot-game-2026-11-11.zip`（ot 工具链 + adofai 音游，2026-10 用户态线）
  按"内核已有的从 ot 去掉、没有但有价值的内核态实现"收编。
- **#92**：CE ABI v2.14 追加 slot 64 `pollkey()`（非阻塞按键）；adofai 的
  "屏蔽 IRQ1 + 直读 PS/2 端口" ring0 hack 全删。
- **#93**：shell 新增 `cat`/`rm`/`cp`/`mv`（权限走 ACL，cp 归属创建者 0644）；
  ot 命令表中 30 个与 shell 重复的命令移除。
- **#94**：ot 收编为 `extapp/ocntools.c` → `/bin/ocntools.ce`（脚本引擎/文件管理器/
  十六进制编辑/搜索/版本控制/镜像检查保留），修了 `.text.ce_entry`、全局 `__ce_api`、
  NULL api 解引用三处遗留。
- **#95**：adofai 收编为 `extapp/adofai.c` → `/bin/adofai.ce`，节拍音效走 v2.11
  `snd_tone`（无声卡静音不报错）；关卡 DEMO1/DEMO2.TAD 随镜像。
- 新回归套件 `tests/ot_regress.py` 16/16，全套无回归（见问题清单 17.1）。

---

## 三、回归总账（#95 后同 ISO 全套）

```
实机: ot_regress 16/16  qemu_regress 12/12  user_regress 15/15  diag_regress 4/4
      disk_regress 5/5  vprobe_regress 10/10  snd_regress 6/6  gfx24_regress 0 failed
      heap_regress 4/4  go_regress 7/7  qa_probe_final exit=0
宿主: test_mkce 48  test_cc 10  test_app_list 23 apps  ce_regress 11P/16S/0F
```

## 四、已知遗留（详见问题清单）

- spawn 不查 `PERM_EXEC`；口令哈希为 djb2（非密码学）；单 gid 无多组。
- 12.2 观察：syscall 内 static 缓冲靠 `g_in_syscall` 非抢占保证安全，将来允许
  syscall 让出 CPU 时须分槽。
- 判定重写级、不做：恒等映射/独立地址空间、SMP/抢占、内存动态化（理由见 ROADMAP 第三节）。

## 五、下一步（对应 ROADMAP 第四节）

P1：spawn 执行权限、多组支持、口令哈希升级、shell 作业控制（`&`/`jobs`/`fg`）。
P2：用户态 libc 雏形、自举环闭环（cc_self 编译自身）、LFN 长文件名、目录操作命令。

## 六、平台线（feat/smp，追加任务 C）

### ACPI 只读表遍历（已落地）

- 新增 `src/acpi.c` / `include/acpi.h`：RSDP 搜索（EBDA 指针 + 0xE0000–0xFFFFF，16B 对齐）
  → RSDT(4B 项)/XSDT(8B 项) → FACP、APIC(MADT)。MADT 解析 Processor Local APIC(type 0)、
  IO APIC(type 1)、Interrupt Source Override(type 2)，供后续 SMP 取用。
- `paging` 暴露 `paging_map_top()`：identity-mapped PSE 窗口上界（可高于帧分配器 `mem_top`，
  尾部 4 MiB 固件保留块被映射但不参与分配）。ACPI 固件指针按映射上界校验；分配器边界不变。
- shell 新增 `acpi` 诊断命令：打印 RSDP/FADT/MADT 物理地址、LAPIC 基址、CPU(APIC ID/使能)、
  IOAPIC(基址/中断基)、IRQ override、FADT 复位块。
- 实测（QEMU 6.2，`-smp 2` / `-smp 4`）：`acpi` 列出 2 / 4 个使能 CPU，LAPIC `0xFEE00000`，
  IOAPIC `0xFEC00000`，override 0→2、5→5、9/10/11。
- 边界：本机 FADT 为 ACPI 1.0 修订（无 ResetReg GAS），复位块标注不可用；关机路径仍由
  `power.c` 的 PM1_CNT / legacy 端口兜底承担，本次未改动。DSDT AML 不解码（沿用 power.c 字节扫描 `_S5_`）。
- 门禁：E3 2/2、F11 9/9、链接布局 `--strict` RC=0。vprobe 在本环境基线即出现 post-`fr` 用例
  超时空屏（基线同复现，判为环境性 flake，非本次回归）。

#### ACPI PM1 关机复核（feat/smp 追加）

- 两个实质修复：(a) FADT `PM1a_CNT_BLK`/`PM1b_CNT_BLK` 偏移由误写的 0x48/0x4C 更正为
  0x40/0x44——ACPI 1.0 FADT 在 0x38/0x3C 放 PM1 EVT 块、0x40/0x44 放 CNT 块，旧代码读到的
  是 PM2_CNT/PM_TMR（0），ACPI 路径从未触发。(b) `power.c` 物理守卫由 `paging_phys_top()`
  （帧分配器上界）改为 `paging_map_top()`（identity PSE 窗口上界）：FADT 位于高位保留 RAM，在
  分配器上界之上但在映射窗口内，旧守卫直接拒读。
- 新增 `poweroff` shell 别名（与 `shutdown` 同 dispatch / CAP_SYS 权限 / 两个命令名 allowlist）；
  `acpi_pm1()` 把 PM1a/PM1b 暴露到 `acpi` 诊断；`acpi_poweroff()` 作为 FADT PM1 路径入口，
  `power.c` 固定端口（0x604/0xB004/0x4004）保留为 ACPI 表缺失时的 legacy 兜底。
- 实测 QEMU 6.2：`-smp 2` `poweroff` 后 qemu 自行退出 rc=0，串口
  `[acpi] poweroff SLP_TYP=0 PM1a=0x604 PM1b=0`（SLP_TYP 先取 `_S5` 扫描值，退路 5、7）；
  `-smp 1` 起机到提示行不回退。VMware/UEFI 真机复核仍待（云端禁关机）。
- 不做：DSDT AML `_S5_` 解释器（沿用字节扫描取首个小整数，取不到回退 5）；ACPI 1.0 FADT 无
  ResetReg GAS，复位块仍标注不可用。

### SMP 拓扑观测（部分落地，AP/多核调度 deferred）

- 新增 `src/smp.c` / `include/smp.h` + shell `smp` 诊断命令。只读观测：MADT CPU 列表、
  CPUID leaf1 BSP APIC ID、IA32_APIC_BASE MSR(0x1B) 基址/使能位、CPUID APIC 特征位；
  每核状态（BSP online / AP deferred），附 UP 调度器存活任务数与 idle ticks。
- 实测 QEMU：`-smp 1` → 1 CPU；`-smp 2` → 2 firmware / online 1（CPU1 deferred）；
  `-smp 4` → 4 firmware / online 1（CPU1/2/3 deferred）。LAPIC `0xFEE00000` enabled=yes。
- **不做 + 理由**：INIT-SIPI-SIPI 起 AP、IOAPIC 重定向表接管中断、多核任务调度均未接入。
  本内核为深度 UP 假设：单 `cur_task`、单 `idle_stack`、单 GDT/TSS、协作式调度；
  AP 上线需低内存 trampoline + 每核 GDT/TSS/栈 + IPI + 可抢占调度器，属问题清单 E4
  重写级（12+ 文件 / 147 处全局状态）。AP 留在 parked，不进调度器，避免破门禁。
  8259 仍为唯一中断路径，LAPIC/IOAPIC 未重编程。
- 门禁：E3 2/2、链接布局 strict RC=0；`-smp 1` 起机不回退。

### AHCI（D4，部分落地；IDENTIFY/DMA 未打通）

- 新增 `src/ahci.c` / `include/ahci.h`，shell 接 `ahci` 探针命令；`ata.c` 驱动表加
  `backend`(0=legacy IDE PIO / 1=AHCI) 与 `ahci_port`，`ata_scan()` 先探 4 个 legacy IDE 槽，
  再调 `ahci_scan()` 把 AHCI 端口追加进同一张表，`ata_read/ata_write` 按 backend 分派（AHCI 优先）。
- 已实现：PCI 配置空间扫 class 0x01 / subclass 0x06 / prog-if 0x01，取 BAR5(ABAR) MMIO；
  ABAR 经 `paging_map_top` 映射；读 `CAP`/`PI`；端口 `PxCLB/PxCLBU/PxFB/PxFBU/PxIS/PxIE/PxCMD`
  初始化（FRE→ST），PHY up（SSTS.DET）检测；命令头(16B)/命令表(128B)/PRDT/FIS 接收区/IDENTIFY
  缓冲均来自 `frame_alloc()` 页对齐帧（不入大 `.bss`）；命令路径含 Register H2D FIS 下发、
  CI 轮询、TFD BSY/DRQ/ERR 等待。
- **遗留（未打通，🔶 部分）**：QEMU 6.2 上 `-device ahci,... -device ide-hd,...,bus=ahci.0`，
  HBA/ABAR/PI/PHY 正常（ABAR `0xfebf1000`，PI `0x3f`，端口0 SSTS `0x113`），但
  `IDENTIFY DEVICE`(0xEC) 下发后 PxTFD 错误字节=0x01(ABRT)、DMA 缓冲全零；READ/WRITE DMA EXT
  (0x25/0x35) 未验证。命令被 HBA 收走（PxCI 清 0）但设备拒命令；根因未定位，怀疑命令表/PRDT
  帧落在用户 arena 物理切片(0x28xxxxx) 导致 DMA/别名异常，或 FIS/命令头布局细节有误。
- 兼容：无 AHCI 控制器时 `ahci` 命令打印 `no AHCI controller`；legacy IDE PIO 路径未回退
  （`-drive if=ide` 读样本盘 `/samples` 正常列出 4 个文件）。
- 门禁：当前构建 E3 2/2、F11 9/9、链接布局 `--strict` RC=0；vprobe 环境性 flake（基线同现）。

---

*本文件随每轮交付滚动更新；历史汇报见 git 历史（`git log -- docs/PROGRESS.md`）。*

---

## 六、截图审计轮（#97~#102，2026-11-12）

12 张 QEMU 实机截图审计出 6 个问题，全部修复（详见问题清单第十九节）：

- **#97（P0）** ACL 路径大小写绕过：`fs_canon_path()` 规范化 + 大小写不敏感比较，
  `/BIN/X.CE`、`./x.CE` 等变体不再绕过 chmod。
- **#98** `cmd_chmod`/spawn 未初始化变量（栈垃圾写入 ACL）。
- **#99（P0，流程）** 构建链骗测：patch_work_iso.py 拒绝打包陈旧 kernel；
  `build.sh iso` 补 `build_apps` 依赖。
- **#100** vdiag 残影/调色板污染（glyphA 一次性输出 + AC 重触发）。
- **#101** CJK 全角标点字形 6961 个（Unifont A1/A3 行 + punctest.ce 探针）。
- **#102** build.sh iso 目标依赖修复。
- **澄清（非 bug）**：磁盘镜像 MBR 无引导代码，引导介质是 catos.iso
  （GRUB isohybrid 双格式）。VMware 引导工件见 `vmware/`（vmx + vmdk
  描述符 + README），isohybrid 当 IDE 硬盘引导已实测到 shell。

### 回归总账（2026-11-12 同 ISO 全套）

```
宿主: test_mkce 48  test_cc 10  test_app_list 24 apps  ce_regress 11P/17S/0F
实机: qemu_regress 12/12  user_regress 15/15  ot_regress ALL  diag_regress 4/4
      disk_regress 5/5  heap_regress 4/4  go_regress 7/7  vprobe_regress 10/10
      snd_regress 6/6  gfx24_regress 0 failed  security_regress 17/17
```

### 已知遗留

- 口令哈希 djb2（非密码学）、单 gid 无多组（ROADMAP P1 不变）。
- VMware 为 SeaBIOS 同源验证 + 配置工件，未经 VMware Workstation 本机实测
  （本机未装）；待用户在 VMware 里按 vmware/README.md 验证。

## 七、开机体验轮（2026-11-12，v2.17）

- **#103（bug）** 退格 `?` 残影：be_text 把空格子（cp==0）画成 '?'。
  改为仅可打印 ASCII 显示原字符、其余画空格。QEMU 复现→修复→验证
  （输 cba + 3 次退格屏幕干净，tests/qemu_shots/bs2.png）。
- **自启动目录（特性）**：开机不再打印硬编码 banner，改为顺序执行
  `/etc/autoexec/` 下脚本（每行一条 shell 命令，# 注释/空行跳过）；
  空卷自动播种 banner.sh（echo 行复刻原 banner）。新命令
  `autostart` 可随时重跑。shell 分发链重构为 shell_exec() 供交互与
  脚本共用。**注意 8.3 命名限制**：目录名用 autoexec 而非 autostart
  （9 字符 stem 被无 LFN 的 fs_mkdir 拒绝——曾因此静默失效）。
- **新开机 logo**：about 的 ASCII 大字换成块状 CatOS。
- 回归：security 17/17、user 15/15、qemu 12/12（autostart 实测截图
  tests/qemu_shots/fin0.png）。

## 八、v2.19 回流 + v2.20 VMware SVGA II 24 位真彩（2026-11-13）

### 8.1 v2.19 反向同步

云主机（Linux `vm`）完成的 #54 / #57 / #58 / #59 / T52 / #60 与
`SYS_GOTOXY`（槽 65），经下载文件夹两个 zip 反向同步回 `D:\CatOs`，
提交 `3ed5cad`（源码 + 文档 + kernel.bin + 全部 .ce 产物）。
覆盖前原文件备份在 `D:\_v219_backup\`。详见
`docs/TODO-2026-11-12-VMware反馈轮.md` §五/§六。

### 8.2 v2.20 新增

- **VMware SVGA II 驱动**（`src/svga.c` / `include/svga.h`）：PCI `15AD:0405`
  探测、BAR0 索引/数据端口、BAR1 LFB、BAR2 FIFO、模式设置、
  25 Hz `SVGA_CMD_UPDATE`（IRQ0 钩子）、`svgainfo` 诊断命令。
- **双后端**：`be_vbe` / `be_vbe24` 按 PCI 身份在 Bochs dispi 与 SVGA II
  之间选择。SVGA II 是 32bpp XRGB，但 CatOS 的 RGB24 绘图 ABI 不变，
  pitch / 每像素字节数 / 通道字节序全部运行时决定。
- **命令**：`vbe`、`vbe24`（新别名）、`gfx24` 在 VMware 显卡上全部可用；
  `display` 增加 `graphics HW` / `last gfx24 hw` / `hw_bpp` / `stride`。
- **修复 #104~#106**：无图形、`vbe24` 不存在、QEMU `-vga vmware` 因
  dispi 端口误判导致黑屏。详见问题清单第二十一节。
- **连带**：`paging.c` LFB PDE 区间取并集；`extapp/gfx24.c` 不再硬编码
  `pitch==2400`；`gfx_info` 追加 BACKEND/HW_BPP/STRIDE 选择器（append-only）。

### 8.3 回归总账（2026-11-13 同 ISO）

```
宿主: test_mkce 48  test_cc 10  test_app_list 24 apps  ce_regress 11P/17S/0F
实机(-vga std): qemu 12/12  user 15/15  security 17/17  disk 5/5
                gfx24_regress 0 failed
实机(-vga vmware): svga24_regress 34/34 PASS
                   (gfx24/vbe24 逐像素 + ring3 rect/blit + vbe 真彩文本
                    + FIFO UPDATE + text 往返)
```

真实 VMware Workstation 17.6.1 尚需用户开机复测；本机 VM 当前未运行，
`vmware/CatOS.vmx` 光驱已指向新 `kernel_v25/catos.iso`，数据盘上的
`/bin/gfx24.ce` 已原地更新（改前备份 `catos_disk.img.bak-v219`）。

## 九、v2.21 外设与交互 + 光标/黑屏修复轮（2026-11-13）

### 9.1 v2.21 新增

- **e1000 网卡驱动**（`src/e1000.c`）：PCI 探测 → BAR0 MMIO → RX/TX 描述符环
  → 中断 → 接 `net_send/net_recv`；`netdiag` 可核对后端与收发计数。
- **ES1371 声卡驱动**（`src/es1371.c`）：PCI BAR + DMA 环形缓冲 + 采样率寄存器
  → 接 `snd_play()`；与 SB16 并存，探测时按 PCI 身份择一（`sndinfo` 可查）。
- **T51 shell 行编辑器**（`src/keyboard.c`）：↑↓ 历史、←→ 光标编辑、Ctrl+↑↓
  回看滚动、语法高亮；`render.h` 增 `con_get_cursor()` 与滚动回看接口。
- **lang 命令**（`src/lang.c`）：`/etc/lang` 持久化 + 关键消息翻译表。
- **CE ABI**：`ce_abi.h` / `intr.h` / `keyboard.h` / `net.h` / `snd.h` 同步扩口。

### 9.2 本轮修的缺陷（#107~#111，详见问题清单第二十二节）

- **#107 VMware `vbe` 黑屏**：`paging.c` LFB/MMIO 映射加 **PCD（不可缓存）**位。
- **#108 光标不闪 + SVGA 不刷新**：`cpu.s` 把 IRQ0 单独接到 `sched_timer_tick`，
  `con_tick()` / `svga_tick()` 挂在 `isr_irq` 上等于**死代码**；改为在
  `sched_timer_tick()` 里直接调用。光标恢复闪烁，SVGA FIFO 真的在推 `UPDATE`。
- **#109 光标样式**：三个图形后端由 8×8 实心块改为**格子末行 1 px 下划线**；
  有字格子由 `render.c::paint()` 画**背景反色**；文本模式硬件光标 13–14 → 14–15。
- **#110 残光标**：`con_set_cursor()` 保留 `dcx/dcy`；`scroll_one()` 滚动前先擦光标。
- **#111 回归断言过期**：`heap_regress` 的 `ls /bin` 改断言 LFN 长名。

### 9.3 新增逐像素回归 `tests/cjk_width_regress.py`（monitor 4493）

CJK = 2×ASCII 的**逐像素**断言（下划线连续段 = 精确格子标尺）：

| 对象 | 组成 | 期望 | 实测 |
|---|---|---|---|
| 纯 ASCII 行 | 49 格 × 8 px | 392 px | 392 px ✅ |
| 混排行 | 12 ASCII × 8 + 14 CJK × 16 | 320 px | 320 px ✅ |

⇒ ASCII 步进 8 px，CJK 步进 **16 px = 2×ASCII**（1 格宽会是 224 px，3 格宽 416 px，
两者都会判失败）。补齐 TODO 轮 §5.4 的"CJK=2×ASCII 逐像素断言"。

### 9.4 回归总账（2026-11-13 同 ISO，LF 归一后重跑）

```
宿主: test_mkce 48  test_cc 10  test_app_list 24  ce_regress 11P/17S/0F
实机(-vga std): qemu 12/12  user 15/15  security 17/17  disk 5/5  diag 4/4
                heap 4/4  go 7/7  snd 6/6  e1000 0F  es1371 0F
                gfx24_regress 0F  svga24_regress 0F  cjk_width_regress 0F
```

**行尾归一**：v2.19 反向同步带进来一批 CRLF 文件，`core.autocrlf=false` 下
会把整文件改写成 CRLF blob。本轮把 7 个"工作区 CRLF / HEAD LF"的文件归一回 LF
（`render.h` `be_gfx.c` `be_vbe.c` `intr.c` `keyboard.c` `render.c` `heap_regress.py`），
提交差异从 ~2900 行降到 **689 插入 / 109 删除**，且归一前后 `kernel.bin`
大小完全一致（6,662,308 字节）。

**真实 VMware Workstation 仍需用户开机复测**（#107 黑屏、#108 刷新、#109/#110 光标）。

## 十、v2.22 口令加固轮（2026-11-14 跨夜）

- **加盐迭代口令哈希**（`src/user.c`）：新 /etc/passwd 第 7 字段格式
  `$S$<8hex salt>$<8hex digest>`；FNV-1a + fmix32 雪崩，8192 轮迭代，
  每记录独立随机盐（timer_ticks 混地址）。同口令不同用户 → 不同盐。
  明确非密码学边界（内核无密码学原语），目标是抵御离线彩虹表/秒级爆破。
- **旧格式兼容 + 透明升级**：legacy 裸 djb2 记录仍可验证，登录成功后
  原地升级为加盐格式（users_dirty → 下次持久化写盘）；legacy "0"（无口令）
  在 user_db_init 归一为空串。
- **新回归 `tests/pw_regress.py`**：12 步（加盐落盘格式、错误口令/大小写
  变体拒绝、正确口令接受、同口令不同盐、无致命故障）。
- **测试基架 Linux/QEMU6.2 移植**：qemu_driver/qemu_regress 等 6 个驱动
  支持 PATH 查找 qemu-system-i386；镜像统一暂存 /tmp（hpvs_fs 无文件锁）；
  `xp` 地址正则放宽到 16 位；type_text 补 `$` 等按键映射。
- **回归总账（2026-11-14，同 ISO）**：宿主 test_mkce 48、test_cc 10、
  test_app_list 24；实机 qemu 12/12、security 17/17、user 15/15、diag 4/4、
  pw 12/12。

## 十一、v2.23 shell 环境变量轮（2026-11-14 跨夜）

- **env 模块**（`src/shell.c`，无新 syscall，ABI 不动）：`export K=V` /
  `setenv` / `env` / `unset K`；`$K`、`${K}` 在键入行与 /etc/autostart
  脚本行统一展开（`$$` 字面量，未知变量展开为空，sh 语义）；同名单上限
  16 项、key<16、val<64，表满/超长明确报错。
- **$PATH 驱动裸名查找**：`run_from_path` 第 2 段由硬编码 /bin 改为解析
  `$PATH`（冒号分隔、按序、容忍空段）；首启 `env_init()` 落盘默认
  `PATH=/bin`。
- **持久化 /etc/env**（KEY=VALUE，随改随存，风格同 /etc/lang）；
  中英文警告消息走 lang 翻译表（append-only L_ENV_NOSAVE）。
- **CE 应用暂不继承 env**：传环境块需 append-only ABI 新槽，列入待办。
- **新回归 `tests/env_regress.py`**：13 步全过（含 PATH 重定向查找、
  变量删除、持久化、无致命故障）。
- **回归总账（2026-11-14，同 ISO）**：qemu 12/12、security 17/17、
  user 15/15、pw 12/12、env 13/13；宿主 test_mkce 48、test_app_list 24。

## 十二、v2.24 递归文件操作轮（2026-11-14 跨夜）

- **fs 层新原语**：`fs_is_dir()`、`fs_rmdir()`（仅删空目录，`.`/`..` 之外的
  条目存在即拒绝，root 永不可删）。
- **shell 三件套对齐现代 OS**：`mkdir -p`（逐级创建、容忍已存在）、
  `cp -r`（目录树复制，深度上限 6，内容 16KiB 分块）、`rm -r`（递归清空
  + 删目录，拒绝 `/`）。裸 `rm 目录` 明确提示需 -r。
- **#112 修复**：`perm_write_on_parent()` —— mkdir / cp -r 落点父目录必须
  有 PERM_WRITE，非 root 用户再也不能在 /etc、/bin 下建目录。
- **#113 修复**：新增 `acl_unset()`，rm / rm -r / mv 成功删除后清理 ACL 条目，
  杜绝"重建同名文件继承旧权限"。
- **#114 修复**：副本 ACL 改 0755，与"无条目"默认一致，`cp` 后的 .ce 可直接 run。
- **新回归 `tests/fsrec_regress.py`**：12 步全过（嵌套创建、树复制+内容核验、
  树删除、误用防护、悬空 ACL、越权 mkdir、无致命故障）。
- **回归总账（2026-11-14，同 ISO）**：qemu 12/12、disk 5/5、security 17/17、
  user 15/15、pw 12/12、env 13/13、fsrec 12/12。

## 十三、v2.25 图形命令行增强轮（脏单元格 / 滚动锚定 / 字号三档）

- **字符单位脏矩形（v2.25 核心）**：render.c 维护 painted[][]（屏幕真实像素的
  单元格镜像），paint() 逐字节比较，相同内容零显存写入；新增选择器
  CON_INFO_CELLS_DRAWN=16 / CELLS_SKIPPED=17（display 显示计数，实测
  skipped 55→717→1629 持续增长）。painted 随 scroll 同步平移，模式切换/
  清屏/scale 切换时整体失效。
- **滚动锚定 #7**：con_scroll_anchor() —— shell 每次打印提示符前把输入行
  滚回屏幕中间行（带 scrollback 归档），长输出后提示符始终居中。
- **字号三档 #2**：vbe24 后端 set_scale(0/1/2) = 8x8(100x75)/8x16(100x37)/
  16x32(50x18)；ASCII 字形本源 8x8，密集模式取消拉伸、大号模式 2x 放大；
  CJK 恒为 2列x1行（密集模式 16x16 纵向压缩为 16x8），WIDE 不变式保住。
  shell 新命令 `size compact|normal|large`，选择持久化 /etc/size，
  重新进入 vbe24 时自动恢复；CON_INFO_CELL_SCALE=18 / CELL_W/H 动态化。
  reflow 复用既有机制，CON_MAX_ROWS 40→80。
- **测试**：tests/size_regress.py（文本模式计数断言 + vbe24 像素绿带断言，
  端口 4492）。normal-band / dirty-skip 等核心检查通过，compact/large
  三档字形截图经像素验证正确。
- **待查（记 #115）**：偶发 guest 崩溃（QEMU -no-reboot 退出，monitor 断管），
  在 size 回归长流程中非确定性出现，单步手动复现未果；下一步加串口日志
  或 panic 截屏定位。

## 十四、v2.25c 细粒度权限 + 全屏用户管理 UI（需求 4）

- **CAP 六细拆（append-only，宽位仍有效，各门接受 宽位 OR 细位）**：
  CAP_FSADMIN→CAP_FORMAT(0x0080)+CAP_FSCK(0x0100)；CAP_DEVICE→CAP_RAWIO(0x0200)
  +CAP_HWINFO(0x0400)；CAP_SYS→CAP_REBOOT(0x0800)+CAP_DIAG(0x1000)。
  format/fsck/reboot 门已切换到 宽 OR 细；`reboot` 由无门控改为需要
  CAP_SYS 或 CAP_REBOOT（root 例外）。
- **预设用户模板**：admin / operator / developer / viewer / guest；
  useradd 向导首选预设，`useredit` 内 R 键应用预设。
- **全屏接管 UI `useredit`**：con_screen_push()/pop() 整屏快照恢复；
  用户列表（name/uid/gid/caps hex/密码有无）+ 方向键选择 + N 新建 /
  E 细粒度 caps 逐位开关 / P 掩码改密 / D 删除 / Q 退出（退出后
  shell 界面逐字恢复）。需 CAP_USERADM。
- **修复**：user_preset_caps 预设比较条件写反（useq==0 误配 admin →
  caps=0xFFFFFFFF），实机捕获后修正。
- **回归**：tests/caps_regress.py 12 步全过（三命令拒绝、预设 0x400、
  细位放行、TUI 往返、屏幕逐字恢复、无致命故障）。

## 十五、v2.26 USB 支持（需求 5，本批 7 项全部完成）

- **UHCI 主机控制器驱动**（src/usb_uhci.c，~600 行）：
  PCI 扫描 8086:7020/7112（PIIX3/PIIX4 UHCI，含 function 扫描——QEMU PIIX3
  UHCI 在 dev1.fn2）；BAR4 I/O 基址；HCRESET/GRESET 复位序列；1024 项
  1ms 帧表（固定物理区 0x560000，与 es1371/snd DMA 区错开）→ QH → TD。
- **完整 USB 枚举**：GET_DESCRIPTOR(8B) → SET_ADDRESS(1) → SET_CONFIGURA
  TION(1)，控制传输经 SETUP/IN/OUT 三 TD 深度链，轮询收割。
- **HID 键盘**：中断 IN（addr1 ep1 8B boot report）10ms 轮询收割；
  make/break 按 6 槽报告比较；修饰键换档合成 set-1 0x2A/0xAA 等；
  HID usage→i8042 scancode set 1 翻译表（字母三段、数字、F1-F12、
  方向键、Home/End/PgUp/PgDn/Ins/Del），进 kb 统一队列——IME、
  scrollback、rl 编辑器全部无需改动即可用。
- **`usb` shell 命令**：控制器/端口/速度/报告数/错误数；`usb scan` 热插入。
- **调试攻坚记录（全部实机捕获）**：
  1. 只扫 fn=0 漏掉 PIIX3 UHCI（fn2）；
  2. FRBASEADD 是 32 位寄存器，outw 只写低 16 位 → 帧表基址=0；
  3. intr.c isr_irq case 0 是死代码，IRQ0 真入口在 cpu.s _irq_0 →
     task.c sched_timer_tick——usb_poll 原先永远不被调用；
  4. UHCI 控制器会推进 QH element pointer（TD 完成后 QEMU 把 TD[0]
     即 TERM 写进 QH[1]），每次重排 TD 必须同时把 QH[1] 指回 TD；
  5. TD 未武装时 TD[1]=0 被误判为"完成 1 字节"，产生幽灵报告；
  6. SET_ADDRESS 发两次且第二次用错设备地址（addr=1 发给 addr0 设备）；
  7. QEMU `-usbdevice keyboard` 存在时 sendkey 输入只走 USB 设备，
     PS/2 收不到——USB 路径是唯一输入通道（stale 内核验证）。
- **回归**：fsrec 12/12、caps 12/12、user 15/15 全绿；实机验证
  `echo hello-from-usb-keyboard` 全 USB 路径完整回显，reports 递增、
  errors=0。截图 tests/qemu_shots/usb_keyboard.png。

## 十六、v2.27 IME 候选条宽度自适应

- vbe24 large 档（50x18，16x32 cell）实机验证 IME：候选条正确跟随输入
  行、选字 commit（"中"）正确上屏。
- 修复：窄档候选条右缘截断（第 9 候选被切半、分页提示丢失）。现按
  con_cols() 计算每个候选占宽（数字 + 汉字 2 cells + 空格），放不下的
  候选主动跳过（= 翻页仍可达），80 列以上才追加 "(-/= page, esc)" 尾注。
- 回归：fsrec 12/12；normal 80 列候选条完整（9 候选 + 尾注）；large
  50 列完整（8 候选，无截断）。

## 十七、v2.28 TAB 补全 + Ctrl+C 行取消

- **Tab 命令补全**：kb_readline 挂补全钩子（kb_set_completer）；shell 侧
  sh_complete：光标位于命令 token 时对 71 个命令名前缀匹配——唯一命中
  直接补全 + 尾空格；多命中把候选列表画到输入行下一行反白条（不动
  行缓冲）。
- **Tab 文件/目录补全**：光标在参数 token 时用 fs_enum_entries（新增，
  含目录、目录名带 '/' 尾）前缀匹配；含 '"' 或路径中已有 '/' 的复杂
  情形暂不处理（保守 return 0）。
- **Ctrl+C 取消行**：kb_readline 遇 ^C（0x03）清行返回 2，shell 主循环
  打印 "^C" 重开提示符——与 bash 习惯一致。
- **根因教训**：fs_enum_files 跳过目录（FAT 目录过滤），补全需新的
  fs_enum_entries；测试盘上 zz_demo 实际不存在导致两轮误判"补全失效"。
- 回归：fsrec 12/12、caps 12/12、user 15/15 全绿。截图
  tests/qemu_shots/tab_complete.png。

## 十八、v2.30 USB 热插拔自动识别

- **缺口**：-usbdevice keyboard 启动时正常，但运行中插入的 USB 键盘
  （QEMU monitor device_add usb-kbd）导致输入全丢：kbd_port=0 时
  usb_poll 直接返回，新设备永远不会被发现，而 QEMU 的 sendkey 又被
  USB 设备独占——死锁。
- **修复（两段式）**：usb_poll（tick 上下文，不能跑带忙等的 attach）
  每 ~1s 探测各端口 connect status，命中只置 pending_attach；
  usb_service()（新接口）在任务上下文消费 pending 执行完整
  reset+enumerate+arm。调用点：kb_next_scancode / kb_next_scancode_timed
  的等待循环——同时把阻塞等待改为 2-tick 轮询（task_block 在无键生产者
  时永不唤醒，usb_service 永远没机会跑）。
- **实机验证**：-usb 启动 → device_add usb-kbd → 首键触发 attach
  （串口 breadcrumb：probe hit → service attach → attached）→ 后续输入
  完整回显。截图 tests/qemu_shots/usb_hotplug.png。
- 对抗测试记录：模式切换 churn（gfx24/size compact|normal|large/text 确认）
  3 轮 6/6 过；shell 输入 fuzz 60 轮无崩溃；并发后台任务 + scrollback
  深分页往返 + ps 正常；fsrec/caps/user 全绿。

## 十九、v2.31 history 命令

- keyboard 暴露 kb_hist_count/kb_hist_get（0=最老，环形栈换算），shell 新增
  `history` 命令：编号列出本次会话命令历史（与 Up/Down 共用 rl_hist，
  连续重复自动去重）。实机验证 tests/qemu_shots/history_cmd.png。
- fsrec/caps/user 回归全绿。

## 二十、v2.32 无盘 format 死循环修复（#116）

- 对抗测试发现：无 -drive 硬盘时 `format` y 确认后假死 20s+。
- 根因：fat_mkfs 在 ata_identify 失败时虚构 64MiB 卷继续写，每个扇区
  写烧满 ata 层 10M 迭代超时，累计分钟级。
- 修复：fat_mkfs→int（-1 无盘）、fs_format→int、format 命令与 boot
  自动 format 路径报错并继续。带盘 format 2s 正常，无盘立即报错。
- fsrec/caps/user 回归全绿。

## 二十一、v2.33 alias/unalias 命令

- bash 风格别名：alias name=value 设置（同名单写覆盖）、alias 列出全部/
  单条、unalias 删除；主循环对命令词做单层展开（value + 原参数拼接后
  重新分词进 shell_exec），8 条容量。
- 实机验证：alias ll='ls -l' → ll / 正确展开执行；unalias 后 ll 变
  unknown command。截图 tests/qemu_shots/alias_cmd.png。
- 对抗测试：编辑器 IME 全链路（F2 开启 → zhong 候选条 → 选"中"上屏 →
  ^S 保存 4 字节 → gfx24 下 cat 正确显示中文）✓。text 模式 VGA 字库
  无中文字形（cat 中文无输出）为硬件限制，记问题清单 #117 低优先级。
- fsrec/caps/user 回归全绿。

## 二十二、v2.34 df 命令 + #118 簇上界修复 + ls -l

- `df`：FAT 卷用量表（total/used/free/use%），fat_free_clusters 扫描
  FAT 空表项；无卷时报错。
- **#118（对抗测试发现）**：fat_alloc/free_clusters 用 max_clus（FAT 表
  容量）当数据簇上界——小卷上 max_clus 可达数据区 2 倍，可能分配越过
  数据区的簇（卷尾损坏风险），df 也因此 free>total。加 clus_hi()=
  vol_clusters+2 统一替换全部 17 处数据簇遍历上界。
- `ls -l`：长格式（perms/size/name），支持目录与单文件；`ls -l 文件`
  不再报 not a directory。
- 清理 keyboard.c/intr.c 缺失 include 的隐式声明警告。
- 实机验证：df 生命周期（写 40 行文件 used 274→275K）✓；ls -l 目录/
  单文件 ✓（-rw-r--r-- 312 hello.ce）。回归 fsrec/caps/user/qemu 全绿。

## 二十三、v2.35 PS1 自定义提示符

- prompt() 支持 PS1 环境变量：\u=用户 \h=主机 \w=当前目录 \$=提示符
  （root 显 #）；未设置时默认提示符不变。export PS1=... 设置、
  unset PS1 恢复。
- 测试基建：qemu_driver.type_text 补全 20+ 特殊字符映射
  （backslash/@/#+括号等），此前反斜杠与多数符号输入直接丢失。
- 实机验证：PS1=\u@\h:\w\$ 后提示符 root@catos:/#；unset 恢复默认。
- fsrec/user/qemu 回归全绿。

## 二十四、v2.35 后对抗测试补充

- 强化 churn 4/4：vbe24 三档字号 + vbe(16bit) + text 确认切换，无崩溃。
- alias+PS1 组合 fuzz：alias 展开进入图形模式、PS1 残留 unset 恢复 ✓。
- 编辑器压力轮 5/5：保存/重开/追加循环，含中文文件名、带空格文件名、
  40 字符超长路径正确拒绝（路径限制按设计生效）。
- **环境事件**：QEMU 二进制中途消失（Ubuntu 环境重置），表现为回归
  FATAL——重装 qemu-system-x86 后全套回归复测全绿，确认非内核回归。

## 二十五、v2.38 P0 4GB 内存支持（跨夜分片）

- **设计权衡（务实落地，未做内存动态化重构）**：沿用既有“内核虚拟地址
  = 物理地址”恒等映射假设，把 PSE 恒等映射范围从固定低 64MiB 扩展到
  multiboot mem_upper 实际报告的 RAM 上界，封顶 4GiB；帧分配器位图按
  4GiB 上限静态预留（32768 字 / 128KiB BSS），只管理 frames_base..mem_top。
  不引入每帧内核窗口、不取消恒等映射（ROADMAP 诚实清单 #1/#3 的重构项
  仍 deferred）。
- **32 位边界处理**：KB→字节用 uint64 中间量换算，避免 mem_upper<<10
  在近 4GiB 回绕；4GiB 本身不可表示，上界封顶 0xFFFFF000。
- **探测链路排查**：截断点共两处——hw.c HW_MEM_TOTAL_KB 钳到 63MiB、
  paging.c top 钳到 64MiB；均已去除。shell `mem` 命令直读 mbi 本来就如实，
  仅补打 PMM used/free 帧。
- **已知限制（遗留）**：① multiboot mem_upper 只统计 1MiB 以上的连续低
  内存，PCI hole 之上的第二段 RAM（mmap 表区域）本内核未解析，大内存
  客户机显示总量仍小于 `-m` 标称值，属如实反映；② 真 4GiB 客户机可能把
  PCI BAR 放进最后 4GiB 块，当前恒等映射未避让 MMIO，需后续内核窗口方案；
  ③ 本机 cgroup 内存上限 4GiB，无法实跑 `-m 4096`，最大验证档 3840MiB
  （客户机报告低内存 ~3071MiB，PCI hole 所致）。
- **遗留项 #122/#123**：#123 ata_read_sectors DRQ 失败改为返回 -1（与写
  路径对称）；#122 export/unset 加管理员闸门，/etc/size 落盘需管理员
  （会话内仍可临时切字号）。详见 docs/night-P0-report.md。

## 二十六、v2.47 shell did-you-mean 一键确认（y 直跑）

- **功能**：主提示符下 unknown-command 给出最近邻建议后，缓存该猜测命令（命令名 + 原始参数）；下一行裸 `y`/`Y` 即按与直接输入相同的 `shell_exec` 路径回放该命令行，回放前以 `-> <line>` 黄/蓝回显实际执行行；任意其他主循环行覆盖并清除 pending；回放一次性消费后立即清除。
- **隔离**：pending 识别只挂主提示符循环；PE->CE offer、format 闸门、gfx consent、useradd wizard 等内部 y/n 问询走各自 `kb_readline`/`kb_getkey`，不持有 pending 状态，其回答不受影响；autostart 脚本模式（g_script_mode）不 arm。
- **测试**：新增 `tests/shell_guess_regress.py`（两趟：format 后注入 mkpe 样本 PE），16 项断言覆盖 y 直跑、大写 Y、参数携带且不残留、无 pending 时 y 报 unknown、PE 问询 y 不误触发、连续两次错误 pending 覆盖；已接入 `tests/run_full_regress.sh`（端口 4450）。
- **回归**：本轮实测 qemu 12/12、guess 16/16；改动为 shell 主循环附加状态机，未触碰解析/补全/路径查找，其余套件按 v2.46 基线（fsrec 12/12、caps 12/12、user 15/15、disk 5/5、env 13/13、heap 4/4、size 7/7；ce 11 FAIL 为 ce_sim 未建模既有基线）。

## 二十七、v2.48 LFN 写侧健壮性（fat.c）

- **非法字符过滤**：新增 `fat_name_illegal()`（fat.c/fat.h），拒绝控制码与 FAT 保留标点 `" * / : < > ? \ |`，以及尾随空格/点；`fs.c` `create_path()` 用户态入口与 `fat_create_nl()` 内核侧双重校验，非法名返回 `FS_ERR_INVAL`（shell 报 `invalid name`）。
- **别名碰撞核对**：`make_alias83()` 按 Windows 短名规则从词干取前 6 字符时剔除点号/空格，非法可打印字符替换为 `_`；`~1..~99` 递增前逐个 `fat_lookup()` 核对 8.3 命名空间（既有 LFN 条目的别名即其短条目，8.3 扫描同时覆盖两种布局）；99 个别名耗尽返回失败。
- **失败路径回滚安全**：`dir_put_run()` 记录每个已写簇与槽位区间；中途 ATA 失败或目录链越界时，把已写槽位逐个墓碑化为 `0xE5` 并重写回盘，不留悬挂的半截 LFN run / 半写短条目。`dir_write_lfn()` 链尾追加新目录簇失败时，解除父簇链接并释放新簇，不再留下孤儿链项；数据簇分配失败仍由调用方 `fat_free_chain()` 回收。
- **lcase 字节（ntres, offset 12）**：新增 `ntres_from_name()`，无损 8.3 条目按原始大小写写 `0x10`（词干全小写）/`0x08`（扩展名全小写）；LFN 别名恒为大写、ntres=0。`dent_init()` 增加 ntres 参数，`fat_mkdir/fat_mkfile` 保留原签名并委托 `_nt` 变体。
- **未做**：原生 rename（copy+delete 现状保留，批次 C）；LFN 上限 240 码元与规范 255 的差距未动；读侧展示仍统一小写（8.3 路径），LFN 路径保留原始大小写。
- **测试**：新增 `tests/lfn_regress.py`（14 项：长名建/列/删往返、7 类保留字符拒绝、同词干别名碰撞共存、多点名往返、混合大小写 8.3 与 LFN 两路径、无致命 fault），接入 `run_full_regress.sh`（端口 4712）。
- **回归**：本轮实测 lfn 14/14、qemu 12/12、fsrec 12/12；纯 fs 层改动，未触 CE ABI 与 shell 主循环。

## 二十八、v2.49 libc.a 雏形——公共 CE 应用层（ce_app.c/ce_app.h）

- **现状收敛**：此前 32 个 CE 应用各自重复同一段入口样板（`__ce_api` 槽定义 + `#include ce_abi.h`），字符串/控制台辅助有的走 vsyscall 槽、有的 app 内手写。本轮抽出公共层 `extapp/ce_app.c` + `ce_app.h`：槽定义只留一份（weak 符号，未改造的旧 app 强定义覆盖之），头文件提供 `ce_write/ce_puts/ce_putc/ce_cls/ce_strlen/ce_strcmp/ce_strcpy/ce_atoi` 内联包装——优先走内核字符串槽，槽缺失时回落纯用户态实现。
- **构建方式（取改动小的方案）**：暂不做 ar 归档与独立 crt0；`build_ce.sh` 在既有 `ce_stub.o` 旁多编一个 `ce_app.o` 链接进每个镜像。归档库（libc.a）与启动代码分离列为后续小设计。
- **首批改造 4 个典型应用**：`hello.c`、`spinner.c`、`sysinfo.c` 移除本地槽定义、改引 `ce_app.h`；`demo.c` 除槽收敛外，4 处字符串调用（strcpy/strlen/strcmp/atoi）切换到公共包装。其余 app 暂保持原样，weak 桥接保证链接不破；cc/cc_self/ocntools 等有自定义类型或双定义的特例本轮不动。
- **测试**：全量重建 42 个 .ce 无重复定义/链接错误；真机探针 hello/spinner/sysinfo 三应用均正常出输出、无 fatal fault。
- **回归**：本轮实测 qemu 12/12、fsrec 12/12（内含改造后 hello.ce 内容完整性校验）；纯用户态构建侧，未触内核。

## 二十九、v2.50 递归操作语义补齐（shell.c / fs.c）

- **覆盖/合并语义**：`cmd_cp` 新增 `cp_target()`——DST 为已存在目录时归一化为 `DST/<src 基名>`（POSIX 合并语义），文件/目录两分支都走归一化目标；否则原样作为覆盖/改名目标。
- **权限位保留**：`cp_src_mode()` 查源 ACL 模式位，`copy_rec` 目录分支与 `fs_copy_file` 文件分支都把源模式位带到目标（owner 仍为创建者；`cp -p` owner 保留未实现）；无 ACL 条目回落 0755。
- **rmdir 命令**：新增 `cmd_rmdir`，拒绝 `/` 与非目录；`fs_rmdir` 返回码映射 -3 非空 / -2 非目录或根 / -1 不存在；命令派发、help、两处补全表已接线。
- **fs_rmdir 拆分 strict/force**：`fs_rmdir`（strict）供 `rmdir` 命令做非空拒绝；`fs_rmdir_force` 供 shell 既有 `rm -r` 后序清空器复用，行为与 v2.24 基线一致。
- **未做 / deferred**：mtime 时间戳保留——fs/fat 层不暴露 mtime 读/写 API，`fs_entry_t` 无日期字段，ls 不显示时间；需新增 stat + set-time 接口（设计级，后续批次）。深度上限 `FSREC_DEPTH_MAX=6` 保留未放宽（放宽需先评估栈风险：每帧两个 FS_PATH_MAX 缓冲）。
- **测试**：新增 `tests/recur_regress.py`（10 项：cp -r 合并嵌套、文件入目录、000 模式保留、对照保留执行位、rmdir 空目录删除 / 非空拒绝 / 文件拒绝、无 fatal fault），接入 `run_full_regress.sh`（端口 4713）。
- **回归**：本轮实测 recur 10/10、fsrec 12/12；qemu 12/12 在 force 拆分前一轮实测通过（拆分后仅 rm_rec 切到 force 变体，未再单跑 qemu）。

## 三十、v2.51 口令哈希加固（user.c）

- **摘要加宽 32 → 128 位**：四条 FNV-1a 车道，各自独立种子常量，逐轮混入口令/盐/轮次，轮内做跨车道异或扩散（车道不是四条独立可并行碰撞的弱拼接），末轮逐车道 murmur3 fmix32 雪崩；记录 digest 32 hex。
- **盐熵源加强**：旧 32 位盐（tick×质数 ^ 记录地址 ^ 口令地址）升级为 64 位双字，混入 IRQ0 tick 相位、RTC 年月日时分秒、记录/口令指针地址，两字分别过 `avalanche32`；盐 16 hex。
- **轮数可配置化**：`PW_ROUNDS_DEFAULT=8192`，静态全局 `pw_rounds` 作为后续可调旋钮；本轮不改默认计算成本。
- **记录格式**：v2 `$S$<16hex salt>$<32hex digest>`；`pw[]` 缓冲 24 → 64。
- **兼容与透明升级**：v1 `$S$<8hex salt>$<8hex>` 旧记录用保留的 32 位混合校验，成功后原地重算 v2；legacy bare djb2 记录同样校验通过后升级。
- **未做 / deferred**：Argon2/scrypt 等内存硬度方案（设计级，内核无 crypto 原语）；轮数运行时管理旋钮未接 CLI；v1/djb2 升级路径无金属探针（测试磁盘上无旧记录可注入，代码路径已评审）。
- **测试**：`user_regress.py` 新增 5 项（set、v2 格式正则、错误口令拒绝、正确口令登录、会话身份），实测 20/20；qemu 12/12。

## 三十一、批次 A 全量回归 sweep（v2.48–v2.51）

- 逐套件前台分次跑，全绿：qemu 12/12、guess 16/16、fsrec 12/12、lfn 14/14、recur 10/10、caps 12/12、user 20/20、disk 5/5、env 13/13、heap 4/4、size 7/7。
- ce_regress 11 FAIL 为 ce_sim 未建模既有基线，不计回归失败。
- 四项 commit：ee6bfcd（v2.48）、80cb16a（v2.49）、ed61383（v2.50）、671c827（v2.51），均已推 origin/main。

## 三十二、v2.52 shell 作业控制最小闭环（task.h / task.c / shell.c）

- 任务层：新增 `T_STOPPED` 状态、`pgid`（独立作业 == tid）与 `pending_sig`；最小信号抽象 `task_signal()`（SIGCONT/SIGTSTP/SIGINT/SIGTERM/SIGCHLD），TSTP 仅允许 READY/RUNNING/SLEEPING（I/O 等待态拒绝，防 CONT 恢复后误读键）。
- shell 内建：`fg [%]TID`（恢复到前台并 wait，停止→前台运行→退出全程闭环）、`bg [%]TID`（后台续跑）、`stop [%]TID`（挂起）；无参数默认最近一个 `&` 任务（g_last_bg_tid）；拒绝对 shell 自身发信号；help/补全表/jobs 状态列（stopped）全部接线。
- 前台 Ctrl+Z（0x1A）：SYS_GETCH 路径拦截，前台任务自挂起、^Z 不下发应用，CONT 后循环回读真实按键；后台任务保留原始字节。
- 修复隐藏竞态（问题清单 #156）：`task_exit_current`/`task_die_fault`/`task_kill` 先 wake parent 再 `maybe_autoreap`，父进程被置 READY 后 autoreap 把僵尸直接回收成 T_UNUSED，新的 `task_wait_child_or_stop` 只认 ZOMBIE/STOPPED 导致 shell 永久等待已回收槽位（原 `task_wait_child` 接受 UNUSED 掩盖了该竞态）。修复为「先 reap 判定、后唤醒 parent」三处统一，wait 循环防御性接受 UNUSED。
- 回归：新增 tests/jobctl_regress.py 12/12（bg 生成/jobs 状态流转/stop→bg→stop→fg 全闭环/无任务报错/垃圾参数/faultinfo），已接入 run_full_regress.sh；全量十二套全绿：qemu 12/12、guess 16/16、jobctl 12/12、recur 10/10、fsrec 12/12、lfn 14/14、user 20/20、caps 12/12、disk 5/5、env 13/13、heap 4/4、size 7/7。

## 三十三、v2.53 多组支持：附加组 + 组数据库（user.h / user.c / shell.c）

- 组数据库：常驻内存表 `ggroups[GROUP_MAX]`，持久化 `/etc/group`，行格式 `name:gid:member,...`；成员按用户名存，`group_db_init()` 加载期对 `/etc/passwd` 解析成 uid；`group_add()` 建组/追加成员，`group_save()` 重写磁盘，`group_info()` 供 `groups` 命令遍历，`user_in_group(uid,gid)` 反查。
- 权限遍历：`fs_perm_check` 组分支先比主 gid，未命中再经 `user_in_group` 遍历任一附加组；owner / primary-gid / supplementary / other 四级。空组库与老单 gid 记录行为向后兼容，未做每任务附加组凭据数组（留作后续）。
- shell：`groupadd GROUP [MEMBER...]`（CAP_USERADM，建组并/或追加成员，持久化）、`groups`（列组与成员名）；help/补全表接线。
- 修复位编码混用（问题清单 #157）：文件 mode 是 octal triad（r=4/w=2/x=1），syscall need 是 PERM_READ=1/WRITE=2/EXEC=4；原先直接相与导致 4xx 读位误拒、执行位误放。取出 triad 先翻译成 PERM_* 再比对 need。
- 回归：新增 tests/group_regress.py 10/10（建组列表/成员经附加组可读/非成员 640 被拒/追加第二成员可读/持久化复登/无 fatal），已接入 run_full_regress.sh；保护复绿：qemu 12/12、user 20/20。

## 三十四、v2.54 轻量守护原语：daemon 标记 + 自启服务（task.h / task.c / shell.c / extapp）

- 任务层：`task_t` 新增 `daemon` 标志，`task_spawn_ce` 槽位复用时复位；守护进程沿用既有后台作业管线（输出捕获 ring + timer/子退出/键入三个既有唤醒源），不引入新调度类或 POSIX 会话模型。
- shell：`daemon CMD [args...]` 内建（run_from_path 后台派生并打 daemon 标记）、`jobs` 列表加 `D` 状态列、help/补全表/dispatch 接线。
- 自启服务：`/etc/autoexec` 行首 `@` 前缀标记守护启动（boot_autostart 走 run_from_path + daemon 标记，uid=UID_ROOT），普通行行为不变。
- 示范守护：`extapp/memlog.c`（周期 `brk(0)` 查询堆水位写入捕获 ring，`log TID` 可读回，`kill TID` 停止）；spinner 上限 8/60→200（jobctl_regress 驱动 `spinner 180` 观测 stop/fg/bg 状态机所需）。
- 回归：新增 tests/daemon_regress.py 5/5（公告 tid/jobs 带 D 标记/log 读回水位样本/kill 回收/无 fatal），已接入 run_full_regress.sh（端口 4467）。
- 探针修复三则（均为断言侧，非内核语义）：开机后 PS/2 幽灵按键 settle（丢弃性 clear + 2.5s）；jobs/kill 断言前 clear 防全屏旧行污染（dm-3 的 log 输出、jc-2/3/4 的历史 jobs 行都含目标词）；短寿命后台任务在断言前自退——`daemon memlog 60 500`（30s）替代 `6 500`（3s），`fg` 目标 `spinner 20`（10s）替代 `spinner 4`（2s）。

## 三十五、v2.59 完整蓝屏崩溃屏（bsod.c / diag.c / intr.c / shell.c）

- 自包含崩溃模块：新增 `kernel_v25/include/bsod.h` + `src/bsod.c`。进屏后只走 `vga_*` 前端 facade（`vga_set_color`/`vga_clear`/`vga_write_at`/`vga_write_at_utf8`）绘制，不触堆、FS、shell、IME、字体加载；键盘绕过 shell/IME/`keyboard.c` 解析栈，直轮询 8042 端口 0x64/0x60（忽略 break code 0x80）；超时 30 s（`BSOD_TIMEOUT_SEC`，TSC 计时，`mhz*1e6*30`）。
- 视觉：整屏蓝底白字（CON_WHITE/CON_BLUE），顶部 ASCII `:(`，标题「CatOS 遇到严重问题，需要重新启动。」，列出 STOP 代码、失败任务名/tid/ring、EIP/CR2/ERR/EFLAGS、tick、panic 消息；末尾提示「按任意键重新启动；按 F1 显示已保存崩溃信息。」F1 第二页转储 `faultinfo` 已保存记录。
- STOP 代码固定映射表（向量 → Windows 风格标签）：

  | 异常向量 | 标签 |
  |---|---|
  | 0 (`#DE` 除零) | `DIVIDE_BY_ZERO` |
  | 6 (`#UD` 非法指令) | `INVALID_OPCODE` |
  | 13 (`#GP` 一般保护) | `IRQL_NOT_LESS_OR_EQUAL` |
  | 14 (`#PF` 页故障) | `PAGE_FAULT_IN_NONPAGED_AREA` |
  | 其余 | `UNEXPECTED_KERNEL_MODE_TRAP` |

- 模式与接入：`/etc/panicmode`（取值 `reboot`/`halt`，默认 `reboot`）启动时 `bsod_init()` 缓存到 BSS，崩溃路径不再打开文件。`panic()` 全面改写：原 msg 带到崩溃报告行，随后进蓝屏；`intr.c` ring0 致命异常记录 fault + serial breadcrumb 后调 `bsod_enter()`；**ring3 任务异常行为不变**（仍只记录 + `task_die_fault` 杀任务，不蓝屏）。
- shell 内建：`bsod`（别名 `blue`）真机渲染一次崩溃屏；`bsod --no-reboot`（`--soft`）测试钩子，按键后清屏回 shell 而不重启；非 soft 与 `panicmode` 写操作需 root 或 `CAP_SYS`/`CAP_REBOOT`。`panicmode [reboot|halt]` 读/写当前模式并持久化 `/etc/panicmode`。
- gfx 兜底：若当前在 gfx24/VBE/Mode13h，进屏前先 `con_force_text()` 应急切回 80x25 文本；切换失败则退化到当前后端的最小可用绘制。
- 自测修复两则（问题清单 #194/#195）：TSC 超时倍率千倍错（30 s 误算 30 ms，进屏即超时退场）；直写 0xB8000 与前端 `redraw_all()` 冲突（直写行被旧 grid 全量重绘盖掉），崩溃屏改为纯前端路径。
- 回归：新增 `tests/panic_regress.py` 11/11（文本渲染/STOP/EIP/回 shell、gfx24 强制切回文本渲染/回 shell、ring3 崩溃隔离系统存活 + faultinfo 记录、halt 模式按键后停蓝屏不复位），已接入 `run_full_regress.sh`（端口 4546）。全套 sweep 复跑保绿：qemu 12/12、guess 16/16、fsrec 12/12、lfn 14/14、recur 10/10、jobctl 12/12、group 10/10、daemon 5/5、panic 11/11、caps 12/12、user 20/20、disk 5/5、env 13/13、heap 4/4、size 7/7；`ce_regress` 11 FAIL 为 ce_sim 未建模既有基线，不计回归失败。sweep 首轮 group/size 各有一次并发端口争用导致的连接重置，隔离单跑后均全绿。
## 三十六、v2.59 坏 FAT 目录项净化 + 目录自检（fat.c / fs.c / shell.c / fat.h / fs.h）

- 显示层净化：新增统一判定 `fat_name_sane(short83, lfn)`、`fat_83_sane()`、`fat_lfn_str_sane()`；8.3 拒绝 NUL/控制字符(<0x20)/非 ASCII(>0x7E)/FAT 保留字符 `" * / : < > ? \ |`，LFN 允许 ≥0x80 UTF-8 字节。`ls`/`tree`/TAB 补全/`fs_enum_*` 统一走 `fat_list_dir_view()` 遍历器；非法名默认隐藏不打印，命令末尾汇总 `N hidden invalid entries (use ls --raw to show)`。
- `--raw` 显式诊断：`ls --raw`/`tree --raw` 列出所有存活条目，坏项红色追加 `c=<cluster> o=<offset> first=0x<hex> raw=<11 字节>`；新增 `fs_enum_entries_raw()` 原始枚举器。合法项行为完全不变。
- 内建 `fsck` 目录自检：`fat_fsck_scan()` 递归扫描整棵目录树（深度 6），分类 `bad-name` / `orphan-lfn`（checksum 不匹配的孤儿 LFN）/ `bad-cluster`（越界首簇），逐项报告路径/槽偏移/首字节/属性/8.3 原始字节；汇总坏项数、可修复数、需重建数。默认只报告，**不删除任何用户数据**；仅显式 `fsck --purge`（`-p`）逐项打印后墓碑化短条目及其前导连续 0x0F LFN 槽。
- 0x05 特例（问题清单 #201）：FAT 规范首字节 0x05 是真实 0xE5 字符的转义；`fat_name_show()` 映射回 0xE5 再渲染，`fat_83_sane()` 判其合法。
- 调试修复（问题清单 #200）：递归扫描器初版复用单一静态簇缓冲，被子目录读覆盖导致外层遍历中断；改为按递归深度分缓冲 `bufs[7][FAT_CLUSTER_SZ_MAX]`。
- 回归：新增 tests/fs_badname_regress.py 23/23（离线注入四类坏项：0x05 合法转义/0x01 控制字符 8.3/stale checksum LFN/保留字符 `?`；覆盖默认隐藏计数、--raw c/o/first、fsck 分类计数、正常目录项不受影响、--purge 后二次 fsck clean、无 fatal）。保护复绿：lfn 14/14、fsrec 12/12、qemu 12/12、user 20/20、size 7/7。

## 三十七、v2.59-crashscreen 增量强化（bsod.c / serial_log.c / shell.c / intr.c / diag.c / gen_symtab.py）

> 基线 cbf77c3（含 bsod.c 284 行、diag.c panic 改写、intr.c ring0 接入）之上做增量，不推翻重写；独立工作仓本地 commit，不 push。可靠性优先、美观其次。

### P0 正确性
- **双输出（P0-1）**：抽出共享轮询串口模块 `serial_log.c/.h`（COM1 0x3F8，等 LSR.THRE 置位，4000 次守卫超时丢弃不阻塞）；关键信息先同步写 COM1，再写 VGA 文本缓冲；任一路失败不影响另一路。`intr.c` 异常面包屑复用同一模块。
- **二次异常护栏（P0-2）**：全局 `g_in_panic` + `g_panic_depth` 深度计数（上限 `BSOD_PANIC_DEPTH_MAX=2`）。`bsod_enter` 入口先 `cli`，见 `g_in_panic` 即深度+1，超限或 triple fault 直接 `cli;hlt` 死循环，禁止递归进蓝屏；`intr.c` `isr_exception` 入口判 `g_in_panic` 直接硬停。软测试路径退出蓝屏时复位标志/深度/记录（#203）。
- **分级核对成表（P0-3）**：`docs/panic-tiers.md` 整理三级——L0 ring3 异常只杀任务 + 一行消息 + 退出码 -1，不进全屏；L1 受控返回码终止相关任务（非异常路径）；L2 ring0 致命异常（#PF/#GP/#UD/双重故障/看门狗）进全屏。看门狗尚未实现（deferred）。代码与文档一致。
- **去依赖确认（P0-4）**：审计蓝屏路径无 `kmalloc`/无阻塞锁/无 sleep/无 FS/无 IME/无 UTF-8 框线；FS 调用仅在启动期 `bsod_init` 缓存模式。内建 GB2312 子集字模表缺标题多个汉字（#202），标题与提示行降级 ASCII；字模为内核内建常量表，证据链见 `docs/panic-tiers.md` §4 与问题清单 #202。

### P1 摘要 / 交互 / 事后诊断
- **摘要页字段（P1-5）**：异常类型 + 十六进制向量（`=== PANIC: CPU EXCEPTION 0x0E (#PF) ===`）、CPU 号、FATAL、RECOVER: NO、简短原因、ERR、EIP、CR2（仅 #PF）、RSP、EFLAGS、tick、EIP 最近符号+偏移、栈顶 3 帧；默认不显示全量寄存器/日志/用户输入/剪贴板。
- **内建符号表（#204）**：`tools/gen_symtab.py` 构建期从 `nm` 读 T/t 符号、去前导下划线、按地址排序，生成 `src/symtab.inc`（`.rodata`，约 785 符号）；`bsod_sym_fmt()` 二分查最近符号+偏移，蓝屏与 `crashlog` 摘要共用；build.sh 编译前先临时链接 PE 取符号（干净树首构兜底）。
- **单键交互（P1-6）**：8042 扫描码直解（只认 ASCII，忽略 break code）：R 触发崩溃动作、D 完整寄存器、S 栈回溯全文、M 内存映射/CR3 摘要、P 排队转储（仅显示 `dump queued`）、A 全核状态（SMP deferred 占位）、C 继续（RECOVER: NO 时拒绝并提示不可继续）；F1 保留 faultinfo 页。输入法强制关、无句子级命令、无子提示符；页面切换整屏重绘，不做增量滚动。
- **转储与事后诊断（P1-7 / #205）**：panic 路径不写 FS；预留 bss 固定区 `panic_record_t`（magic `CPN1`：tick/向量/EIP/CR2/ERR/EFLAGS/ESP/栈顶 5 帧/版本/原因）。下次正常启动内建 `crashlog` 读取并符号化显示摘要；`crashlog --full` 才出原始片段并附敏感数据策略提示；无记录提示 `no crash record`。固定扇区转储 deferred。

### P2 增强（尽力项）
- 标题反显（`!!! PANIC !!!` 白/蓝反显行），无颜色也可读；配色仅增强；蓝屏路径禁光标闪烁/进度条/动画；IME 候选区不进入。
- 窄屏（<60 列）/串口只显示核心摘要（向量/EIP/原因/提示），不做双列布局与栈转储。
- SMP 接口占位（#207）：per-CPU 快照槽位、NMI/IPI 停核标记数组、`[CPU n]` 串口前缀；UP 构建只采样 CPU 0，cpus 页标注 deferred。
- `/etc/panicmode` 扩展支持 `reboot|halt|wait` 与 `reboot:<Ns>` 倒计时秒数形式；启动期缓存，崩溃路径不开文件；开发版默认 `wait`。

### 回归
- `tests/panic_regress.py` 扩充至 19 步：摘要字段完整、D/S/M/P/A 各键翻页可见断言、C 拒绝、串口双输出抓取断言、ring3 隔离 + faultinfo、crashlog 无记录提示、P 仅 `dump queued`、halt 模式留屏。`tests/diag_regress.py` 补 `force_text` 盲切文本。

#### 同 ISO 实机复核（2026-11-18，本工作仓 CatOs-f）
- 全绿：panic_regress **19/19**、qemu_regress **12/12**、caps_regress **12/12**、user_regress **20/20**、fsrec_regress **12/12**、shell_guess_regress **16/16**；diag 4/4、lfn 14/14、badname 23/23、daemon 5/5、disk 5/5、env 13/13、heap 4/4、recur 10/10、jobctl 12/12、group 10/10。
- `ce_regress` 11 FAIL 为既有基线（ce_sim 未建模），不计本轮回归失败。
- **size_regress 未在本仓拿到 7/7**：卡在 `size large` 步 QEMU screendump 捕获（large.ppm 未刷新），本仓未改 `be_vbe24.c`/`render.c`/size 链路（diff 仅触及 bsod/serial_log/shell/intr/diag/gen_symtab 与测试）；主仓基线在同一步同样卡住，判定为既有 screendump 时序/争用问题，非本轮增量引入。端口写死 4492，复跑需避开外部 sweep 占用。
