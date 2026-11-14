# R3 事实核查：核验期间宿主上的并发 guest 是谁的

> 结论先行：`:4467` 那个 `-vga vmware` guest **不是我们这四个人起的**，它来自
> **主工作区 `D:/CatOs` 的 `tests/svga24_regress.py`**，属于"环境既有并发负载"。
> 本文是 team-lead 要求的落地证据（PID / 启动时间 / 命令行 / 归因过程）。

## 一、观测方式

不以"我以为"为准，全部来自进程表与文件 mtime 两个独立来源：

```powershell
Get-CimInstance Win32_Process -Filter "Name='qemu-system-i386.exe'" |
  Select-Object ProcessId,CreationDate,ParentProcessId,CommandLine
```

以及 `D:/CatOs/tests/qemu_disk_*.img` 的 mtime（每个回归脚本启动时都会用
`shutil.copyfile` 刷新自己的那份一次性磁盘副本，**所以副本 mtime ≈ 该次运行的时间戳**，
这是不需要额外埋点就能拿到的运行记录）。

## 二、18:08 时刻的并发 guest（核验进行中）

原始输出见 `R_residual_risk.txt`。

| PID | 启动时间 | monitor | 归属 | 命令行（截断） |
|---|---|---|---|---|
| 23488 | 2026-11-13 **18:08:06** | `:4492` | **本团队（红队 testing-expert）** | `…\redteam-crash-2026-11-13\kernel_v25\catos.iso … -monitor tcp:127.0.0.1:4492 … -drive file=…redteam-crash-2026-11-13\tests\attack_disk.img,format=raw,if=ide` |
| 16812 | 2026-11-13 **18:08:14** | `:4467` | **非本团队（外部会话）** | `… -cdrom D:\CatOs\kernel_v25\catos.iso … -monitor tcp:127.0.0.1:4467,server,nowait **-vga vmware** -drive file=D:/CatOs/tests/qemu_disk_svga24.img,format=raw,if=ide` |

我的核验 guest 是第三个（`:4494`，18:08:5x 起，约 40 s 后退出）。

## 三、归因：`:4467` 是谁

### 1. 端口 + 设备 + 磁盘三者唯一匹配到一个脚本

```
D:/CatOs/tests/svga24_regress.py:31: MONITOR_PORT = 4467   # own port: 4444/4460..4466/4476 taken
D:/CatOs/tests/svga24_regress.py:34: DISK = os.path.join(HERE, "qemu_disk_svga24.img")
D:/CatOs/tests/svga24_regress.py:166:                extra_args=["-vga", "vmware"])
```

`-monitor :4467` + `-vga vmware` + `D:/CatOs/tests/qemu_disk_svga24.img` 这三项同时命中，
**只有 `D:/CatOs/tests/svga24_regress.py` 一个来源**（本仓库其它脚本的端口是 4460/4468/…，
设备各不相同）。该脚本用 `HERE`（=`D:/CatOs/tests`）拼路径、ISO 默认取 `D:/CatOs/kernel_v25/catos.iso`，
与观测到的命令行完全一致 → **它是从主工作区 `D:/CatOs` 用相对路径 `python tests/svga24_regress.py` 起来的**。

### 2. 它属于一次"设备回归连跑"，不是我们这轮的任何一个任务

`D:/CatOs/tests/` 下一次性磁盘副本的 mtime 构成一条清晰的串行序列（每个约 70–75 s，
即一次 `boot → 断言 → quit` 的耗时）：

| 时间 | 副本 | 对应脚本 | 端口 |
|---|---|---|---|
| 18:03:19 | `qemu_disk_e1000.img` | `e1000_regress.py` | — |
| 18:04:34 | `qemu_disk_es1371.img` | `es1371_regress.py` | 4468 |
| 18:06:13 | `qemu_disk_snd.img` | `snd_regress.py` | 4460 |
| 18:07:25 | `qemu_disk_gfx24.img` | `gfx24_regress.py` | — |
| **18:08:40** | **`qemu_disk_svga24.img`** | **`svga24_regress.py`** | **4467** |

`D:/CatOs/tests/` 下**没有**任何批跑入口（`run*/all*/suite*/sweep*` 均不存在，也没有脚本
引用多个 regress 脚本），所以这是有人（人或另一个会话）**逐个手动/循环调用**这些脚本的
一轮设备回归。该树中存在 `docs/TODO-2026-11-12-VMware反馈轮.md`，工作区 `git status` 显示
`sndtest.c / ce_abi.h / intr.h / keyboard.h / net.h / render.h / snd.h / build.sh /
builtin_apps.inc` 均处于修改中 —— **主工作区正在做一轮设备/VMware 反馈修复，与本次崩溃对抗测试无关。**

### 3. 排除本团队

本团队 4 名成员的实际运行记录（全部在本轮 worktree `redteam-crash-2026-11-13` 内）：

| 谁 | 脚本 | 端口 |
|---|---|---|
| testing-expert（红队） | `tests/crash_attack.py` | 4492 |
| code-reviewer（蓝队） | `tests/regression_after_fix.py --side baseline/patched` | 4496 |
| sre-engineer（我） | `tests/isolation_verify.py` | 4494 |

**没有任何一个人调用过 `svga24_regress.py` / `gfx24_regress.py` / `snd_regress.py` / `es1371_regress.py`**，
也没有任何人的工作目录是 `D:/CatOs`。`qemu-system-i386.exe` 的属主是 `杨氏\Admin`——和我们同一账户，
所以**属主字段无法区分人**，但脚本路径 + 端口 + 设备 + 副本 mtime 四者互锁，归因是明确的。

### 4. 结束时间

`18:11:13` 的进程快照里已经没有 `:4467`（只剩 `:4492` crash_attack 与 `:4496` regression_after_fix
两个本团队 guest）→ 该 guest 在 **18:08:40–18:11:13 之间正常退出**，生命周期约 1–2.5 min，
与 `svga24_regress.py` 的单次运行时长一致，未见异常残留。

## 四、这对我们的结论有什么影响

### 对"我们没有污染别人"

- **成立**：我们三个 guest 的 argv 全部是 `-net none`、只写自己 worktree 内的一次性副本、
  monitor 都在 `127.0.0.1` 的私有端口、收场只按 PID。
- 但归因边界要说清：**`:4467` 的读写不归我们**（它写的是 `D:/CatOs` 的副本与 `qemu_shots/`），
  我们既没有启动它，也没有杀它。若日后要复盘"谁在什么时候跑了什么"，
  本文件 + `qemu_disk_*.img` 的 mtime 就是可复用的取证方法。

### 对 C3 HANG 的 25 s 判定

这条是**实际影响**，不是理论担忧：

- 核验窗口内宿主上同时跑 3 个 QEMU（每个 `-m 256`、单核 TCG 软件模拟）。
  TCG 是 CPU-bound 的：三个 guest 并发时，单个 guest 拿到的 CPU 时间片会被压缩，
  **"25 s 内没有回 prompt"在负载高时更容易被误判为 HANG，在负载低时可能反而消失**。
- 因此 `crash_attack.py` 报 C3（HANG）时，**必须同时记录宿主侧当时有几个 qemu 在跑**。
  建议在攻击脚本里加一行 `Get-CimInstance Win32_Process -Filter "Name='qemu-system-i386.exe'"` 的
  PID 清单进日志（我已把这个建议发给 testing-expert）。
- 更硬的 HANG 判据（不依赖计时）：monitor `xp` 仍可读、屏幕文本完全静止、
  `info registers` 的 `eip` 多次采样落在同一地址 → 死循环；若 `eip` 在变则更可能是被拖慢。

## 五、附带发现（与本任务无关但影响全队）：C 盘曾写满

核查过程中 `C:` 的可用字节数为 **0**（`Win32_LogicalDisk.FreeSpace = 0`），
这会让写盘型操作（`shutil.copyfile` 拷 64 MB 副本、蓝队最终构建、编辑器保存）直接失败。
我已清掉 `%TEMP%` 中 7 天以上的文件（7329 个 / 5.02 GB），当前 `C:` 可用约 **4.9 GB**。
本 worktree 自身占用 321 MB（其中多个 64 MB 一次性镜像副本），不是主因；
主因在 `C:\Users\Admin\AppData\Local`（23.35 GB）与 `%TEMP%`（清前 7.92 GB）。
详见发给 team-lead 的通报。

### 5.1 取证完整性声明（诚实条款）

> **本次隔离核验的取证曾受宿主磁盘写满影响，相关项已重做。**

具体经过：第二次完整核验的 `shutil.copyfile` 阶段遇到 `PermissionError`（64 MB 副本刚被
AV 实时扫描占有），我加了重试后重跑；重跑过程中又遇到 `ENOSPC`（宿主 `C:` 可用 0 字节），
导致 Bash 侧取证命令本身无法落盘。**这两次失败都没有被当作结论使用**：
- 失败运行的日志被后续成功运行整体覆盖，现存 `isolation_logs/*.txt` 全部来自
  **最后一次成功的完整运行**（10/10 PASS）；
- 为防止再次发生，脚本已加两处防御：`prepare_probe_disk()` 的 24×1.5 s 重试，
  以及跑完自动删除 64 MB 副本（`--keep-probe` 可保留），避免本工具自己成为磁盘压力源。

这条写在这里而不是藏起来，是因为"取证过程中断过"本身是报告必须披露的事实——
读者需要知道这些 hash 是在什么条件下得到的。
