# CatOS vs 通用操作系统差距分析（2026-11-17）

> 基线：HEAD `6cf1ff6`（v2.45），32 位 x86 单内核，QEMU/i386，GRUB multiboot，FAT32。
> 本文档为**纯调研交付**：只列差距、现状定位、通用 OS 做法、评级与今夜可行性，不含内核/应用代码改动。
> 承接文档：`docs/pe-dynamic-linking-design.md`（动态链接 P1 设计）、`docs/ROADMAP-通用操作系统.md`（三档路线图）、`docs/CatOS-内核问题清单.md`（#128~#152 历史项）。

## 0. 口径与评级说明

- **现状**：引用仓库内源码文件 / 函数名 / 行号区间，均已在基线 HEAD 上核实；与任务初判不一致处按源码实际记录。
- **差距评级**：
  - **高**：通用 OS 标配、直接影响可用性/兼容性；
  - **中**：通用 OS 标配但 CatOS 已有部分等价物，缺的是完整性或标准合规；
  - **低**：边角体验 / 加固项。
- **今夜可行性标注**：
  - **可增量**：现有架构内可小步落地，风险可控；
  - **需设计文档级重写**：动核心假设，需先出设计文档再排期；
  - **暂缓（deferred）**：评估过代价，本轮明确不开工。
- 每条目附「今夜预估工作量 / 主要风险」，供父代理按清单派工。

## 1. 调研方法

- 内核源码：`kernel_v25/src/`、`kernel_v25/include/`；用户态应用：`kernel_v25/extapp/`。
- 外部基线：FAT VFAT LFN 规范、POSIX 作业控制与信号模型、口令哈希基线（PBKDF2/bcrypt/scrypt/OWASP）、通用 TUI 组件模型。
- 已完成项不重复列为差距，仅在第 4 节列对照备查，避免重复派工。

## 2. 增量候选差距项（必查清单）

### 2.1 FAT LFN 长文件名（读侧优先）

**现状（基线 `6cf1ff6` 已核实）**

- 读侧（v2.18）：`src/fat.c` 中 `lfn_acc_reset()` / `lfn_acc_feed()`（520–545 行）按序号聚合前置 LFN 条目，`lfn_of_short()`（549–552 行）用别名校验和匹配；目录列举 `fat_list_dir()`（约 870 行）优先返回长名；`LFN_MAX_CHARS 240`（446 行）。
- 写侧：`dir_write_lfn()`（723–801 行）生成 `BODY~N.EXT` 别名并写入 LFN run；`lfn_checksum()`（448 行）按 8.3 别名计算校验和；`dir_write_lfn()` 在目录簇内找连续空闲窗口（719–743 行）。
- 删除：`fat_delete_name()`（817–866 行）把短条目标 `0xE5`，并按校验和回收前置的同 checksum `0x0F` LFN 条目。
- **已核实缺口与待复核边界**：
  1. 无原生 rename 路径：shell `mv` 实现为 copy + delete（`src/shell.c` 2792–2798 行）；改名走新建 LFN run + 删除旧条目，跨目录移动、长名覆盖、删除失败时旧别名链残留等语义未测。
  2. 别名碰撞规则未与 Windows 短名生成规则核对（前 6 字符大写 + `~N` 递增）；别名编号耗尽、8.3 重名冲突时的行为未知。
  3. 写侧跨目录簇的 LFN run 拼接、满目录下连续窗口失败路径未见明确错误处理。
  4. 8.3 条目的 lcase 字节（offset 12）未写；与 NT/Windows 互操作时短名大小写语义可能丢失。
  5. 长名上限 `LFN_MAX_CHARS 240` 与规范 255 码元有差距；文件名非法字符集（`/ \ : * ? " < > |`）过滤未见实现。
  6. 末槽 `0x0000` 终止符与未用字符 `0xFFFF` 填充的写侧细节需对照规范复核。

**通用 OS 做法（VFAT 规范要点）**

- 长名按每槽 13 个 UCS-2 字符拆分；LFN 条目属性字节恒为 `0x0F`（只读+隐藏+系统+卷标），起始簇字段恒为 0；槽位逆序存放，末槽序号 `OR 0x40`；槽内未用字符填 `0xFFFF`，末尾补 `0x0000`。
- 每槽携带 8.3 别名校验和：`sum = (((sum&1)<<7)|((sum&0xfe)>>1)) + name[i]`（遍历 11 字节短名）。
- 别名生成：大写前 6 字符 + `~1`，冲突递增；长名最多 255 个 UTF-16 码元。
- 旧 DOS 工具只识别 8.3 别名、忽略 LFN 条目；非 VFAT 系统上拷贝会丢失长名。
- 参考：Linux kernel `Documentation/filesystems/vfat.txt`；Microsoft FastFAT（The FAT File System）；FAT directory entry 结构说明（tc.gts3.org fat-structs.pdf）。

**差距评级**：中（读侧主路径已通，缺口集中在写侧健壮性与互操作细节）。

**今夜可行性**：**可增量**。第一批做读侧兼容核对、非法字符过滤、别名碰撞与目录满失败路径的单测；原生 rename 路径属小设计，排第二批。
**工作量 / 风险**：小~中；风险低，纯 fs 层改动，不动 CE ABI。

### 2.2 动态链接（承接 pe-dynamic-linking-design.md）

**现状（已核实）**

- CE01 flat 镜像：20 字节头（magic `CE01`、entry、bss_size、api_offset、image_size）+ 扁平代码/数据，固定加载到 `USER_IMAGE_BASE = 0x2000000`；内核在 `USER_VSYSCALL = 0x2080000` 建 vsyscall 页，函数指针表 `VS_TABLE = 0x2080800`，槽位 append-only（现 65+ 槽），老二进制不受影响。
- ELF32 轨道：`spawn_load_elf()`（`src/task.c:1933`）支持 PIE + ASLR，段映射进扩展竞技场 `USER_EXT_BASE 0x2400000` ~ `USER_EXT_END 0x2800000`。
- PE32 轨道：`spawn_load_pe()`（`src/task.c:2086`）只解析 DOS header → PE sig → COFF → Optional header → section headers → entry point；**导入表 / 导出表 / 重定位表一律忽略**（问题清单 #130）；`tests/mkpe.py` 离线把 PE32 翻译成 CE01 flat。
- 不存在的机制：跨任务共享只读代码段（页帧按任务分配）、模块表 / 引用计数 / 卸载路径、运行时链接器（ld.so 等价物）、符号名/序号解析。
- 承接设计：`docs/pe-dynamic-linking-design.md` 已评估三条路线——路线 1 离线/装载期静态重定位（推荐，延续 P1 方向）；路线 2 内核装载期重定位 + 导入绑定 vsyscall 槽；路线 3 用户态动态链接器。

**通用 OS 做法**

- 装载器解析 Import Directory Table / Export Directory / `.reloc` 基址重定位块；GOT/PLT 绑定到真实函数地址；只读 `.text` 被多任务共享映射；模块带引用计数与卸载路径；动态链接器按依赖链递归装载。

**差距评级**：中（教学内核阶段非阻塞，但是生态复用外部二进制的主要天花板）。

**今夜可行性**：**需设计文档级重写**（运行时动态链接）；其中**路线 1 离线重定位可增量**：在 `mkpe.py`（或内核装载器）补 PE `.reloc` 条目（`IMAGE_REL_BASED_HIGHLOW`）按实际加载基址逐条修正绝对引用，外部调用直接绑定 vsyscall 槽位。共享页、模块生命周期、运行时链接器 deferred。
**工作量 / 风险**：离线工具侧小；内核侧重定位循环中；共享页与模块管理触及内存模型，属重写前置。

### 2.3 守护进程框架

**现状（已核实）**

- 任务状态机：`T_UNUSED / T_READY / T_RUNNING / T_SLEEPING / T_WAIT_KB / T_WAIT_CHILD / T_ZOMBIE`（`include/task.h:30–37`）。
- 唤醒源只有三类：timer tick（`task_sleep_ticks()`，`src/task.c:259`；`SYS_DELAY` 1219 行）、子进程退出（`T_WAIT_CHILD`）、按键（`T_WAIT_KB`）。
- 后台常驻任务靠 shell `&` + `delay()` 轮询（范例 `extapp/spinner.c`）；**无通用事件等待原语**（信号量 / 事件标志 / 条件等待队列）、无信号分发、无 daemonize / 会话脱离、无开机自启服务清单。
- 后台任务输出由 shell 捕获为日志，经 `jobs` / `log TID` 查看（`src/shell.c:1590–1643`）。

**通用 OS 做法**

- daemonize：fork + `setsid()` 脱离控制终端、重定向 stdio、切换 cwd、重置 umask；init / systemd 以单元为单位管理常驻服务；事件循环基于 poll/epoll 或事件队列 + 阻塞唤醒原语；信号（SIGHUP 重载、SIGTERM 退出）异步投递到进程。

**差距评级**：中。

**今夜可行性**：**可增量**（轻量版）：内核补一个通用事件等待/唤醒原语（或最小信号量槽），ABI 加 daemon 标记与自启清单文件；完整会话模型与信号分发需与 2.4 作业控制一起设计。
**工作量 / 风险**：中；风险点是阻塞原语与现有非抢占假设、#86 static 缓冲安全模型的交互，需回归调度与后台任务测试。

### 2.4 Shell 作业控制（`&` / `jobs` / `fg` / `bg`）

**现状（已核实）**

- 已有：尾 `&` 后台解析（`src/shell.c:4272–4276`）；`jobs` 内建 `cmd_jobs()`（1603 行，遍历 `t->bg_mode` 任务）；`log TID`（1634 行，查看后台任务捕获输出）；`kill TID`（4221–4229 行，无条件终止，不带信号号）。
- **缺口**：
  1. 无 `fg` / `bg` 内建：后台任务无法拉回前台，被停止的任务无法继续。
  2. 无进程组 / 作业组概念：`task_t` 无 pgid；管道三段（shell.c:1399 起）各自独立调度，键盘中断无法打到整组。
  3. 无信号抽象：CE ABI 无 signal 槽；无 SIGTSTP / SIGCONT / SIGINT / SIGCHLD；Ctrl-Z 挂起、Ctrl-C 组终止均未实现。
  4. 无前台进程组归属：终端驱动不区分前台/后台，后台任务读终端不会被停止（无 SIGTTIN / SIGTTOU 机制）。
  5. 子进程状态只区分退出（`T_WAIT_CHILD` / `T_ZOMBIE`），无 stopped 状态；`wait` 无 `WUNTRACED` 等价物。

**通用 OS 做法（POSIX）**

- 会话（session）+ 控制终端 + 前台进程组；`setpgid()` / `tcsetpgrp()`；每条管道归属一个作业组；键盘信号 SIGINT / SIGTSTP / SIGCONT / SIGTTIN / SIGTTOU / SIGCHLD；`waitpid(WUNTRACED)` 感知 stopped 子进程。
- shell 维护作业表：job id、pgid、PID 列表、状态（running fg / running bg / stopped / done）、命令行；`fg %n` / `bg %n` / `jobs` / `kill %n`；会话退出时向作业组投递 SIGHUP。
- 参考：POSIX XBD §3 Job Control；Bash Reference Manual §7.1 Job Control Basics；VT CS3214 Job Control 讲义。

**差距评级**：中（已有后台雏形，缺的是信号与进程组模型）。

**今夜可行性**：**可增量**（第一批）：`fg` / `bg` 内建 + task 增加 pgid + 最小信号枚举（SIGCONT / SIGTERM / SIGTSTP）；完整 POSIX 作业控制（终端归属、SIGTTIN/SIGTTOU、SIGCHLD 通知）属设计级。
**工作量 / 风险**：中；风险：与调度器、非抢占安全模型耦合，必须回归管道、后台、`jobs/log` 既有测试。

### 2.5 口令哈希升级

> 注意：任务初判为"现在是 djb2 单轮"，源码核实后**现状已在 v2.22 升级过一轮**，本节按源码实际记录。

**现状（已核实，`src/user.c`）**

- legacy djb2 单轮：`user_hash_pw()`（62–67 行）仅保留用于校验旧 `/etc/passwd` 记录，校验通过后原地升级。
- v2.22 方案：`pw_hash_salted()`（78–89 行）FNV-1a 混合 8192 轮，每条记录独立随机盐；记录格式 `$S$<salt8hex>$<digest8hex>`（119–130 行）；校验函数（143–151 行）识别 `$S$` 前缀，旧 djb2 记录验证成功即升级落盘。
- **剩余弱点**：
  1. 摘要仅 32 位（uint32）：离线猜测空间 2^32；8192 轮虽抬高单次猜测成本，但无内存硬度，GPU/ASIC 可并行暴力；源码注释已明示非密码学边界。
  2. 盐熵来源为 timer_ticks × 乘数 + 记录地址（119–122 行），熵源弱于独立 RNG；盐仅 32 位。
  3. 算法标签仅半前缀（`$S$`），迭代参数不可配置；无 pepper；无按新方案 rehash 的迁移触发点（除登录成功路径外）。

**通用 OS 基线（OWASP 2025/2026）**

- 首选 Argon2id：m=19 MiB、t=2、p=1（等价配置 46 MiB / t=1）；次选 scrypt N=2^17、r=8、p=1；bcrypt cost ≥10（仅 legacy）；FIPS 场景 PBKDF2-HMAC-SHA256 ≥600,000 轮。
- 每口令唯一随机盐 ≥128 bit；慢且内存硬；算法标签随记录存储；登录成功时按新方案 rehash 迁移旧记录。
- 参考：OWASP Cheat Sheet Series — Password Storage。

**差距评级**：中（已脱离"明文 / djb2 防窥视"档，但距现代慢哈希基线差距明显）。

**今夜可行性**：**可增量**：摘要宽度扩到 128/256 bit、上调迭代轮数、盐接更强熵源；Argon2/scrypt 等内存硬算法在 32 位小内存内核属设计级，今夜不引入。
**工作量 / 风险**：小~中；风险：必须保留旧记录兼容升级路径；哈希耗时需控制在交互登录时延内。

### 2.6 用户态 libc.a 静态库雏形

> 待填充：现状（ce_stub.c 仅空桩；各应用手写 ABI 初始化与包装）/ 通用 OS 做法（静态归档库 + 统一头文件 + 启动样板）/ 评级 / 今夜可行性。

### 2.7 递归文件操作：`cp -r` 与 `mkdir -p`

> 待填充：现状（v2.24 已有 mkdir -p / rm -r / cp -r，深度受限）/ 通用 OS 做法（递归语义、合并/覆盖、权限时间戳保留、rmdir）/ 评级 / 今夜可行性。

### 2.8 spawn 执行权限检查

> 待填充：现状（v2.15/#99 三轨统一 PERM_EXEC 闸口）/ 通用 OS 做法（x 位 + 文件能力 + noexec）/ 评级 / 今夜可行性。

### 2.9 多用户组支持（supplementary groups）

> 待填充：现状（task_t / user_rec_t 单 gid，/etc/passwd 单 gid 字段，/etc/acl 单 gid）/ 通用 OS 做法（主组 + 附加组、group 数据库、组权限判定）/ 评级 / 今夜可行性。

## 3. 重写级三项（deferred，只给设计要点，禁止今夜强行开工）

### 3.1 取消恒等映射 / 独立用户地址空间

> 待填充：设计要点 + deferred 理由。

### 3.2 SMP / 内核抢占

> 待填充：设计要点 + deferred 理由。

### 3.3 内存动态化（COW / 换出 / 解除堆上限）

> 待填充：设计要点 + deferred 理由。

## 4. 已完成项对照备查（避免重复派工）

> 待填充：逐项列出本轮核实为「已落地」的能力与源码定位。

## 5. 今夜实施优先级排序

> 待填充：按工作量小、收益大、风险低排序，给出派工批次。

## 附录 A：源码定位索引

> 待填充。
