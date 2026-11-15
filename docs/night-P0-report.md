# CatOS 跨夜迭代 P0 分片报告：4GB 内存支持 + 遗留观察项修复

基线 commit：`9e47280 v2.37`　｜　工作副本：`night/CatOs-P0`（本地 main，未 push）
构建：`sh build.sh`（产出 `kernel_v25/catos.iso`），磁盘：`sh build.sh disk`
日期：2026-11-15

## 一、结论

- 内核 PMM / 物理页分配器 / 分页目录不再把内存截到 64MiB，管理范围跟随
  multiboot `mem_upper` 实际报告值，封顶 4GiB；`mem` / `hwinfo` / `top`
  如实显示物理总量与分配器已用/空闲帧。
- 三档实机验证：`-m 256` / `-m 1024` / `-m 3840`（本机 cgroup 上限，见
  遗留问题①）下内存显示如实、`hello` / `allocprobe` 正常运行，gfx24 LFB
  画面正常。
- 遗留项 #123 已修；#122 按问题清单建议的“落盘前加 admin 检查”落地，
  并实机验证非 root 被拒、root 放行。
- 核心回归 qemu_regress 11 PASS；env / disk / user 回归全绿；heap 功能项
  PASS（1 项列表文本断言为既有出屏竞态，见第四节）。

## 二、改动文件清单（源码）

| 文件 | 改动 |
|---|---|
| `kernel_v25/src/paging.c` | PMM/分页扩展到 4GiB 上限；去 64MiB 截断；64 位 KB→字节；位图按 4GiB 预留；任务目录镜像全部 1024 个内核 PDE；frame_free/mark_frame 边界加固 |
| `kernel_v25/src/hw.c` | 去 HW_MEM_TOTAL_KB 的 63MiB 钳制，总量按 bootloader 报告如实计算并封顶 4GiB |
| `kernel_v25/src/shell.c` | `mem` 命令补打 PMM used/free 帧；#122：`export`/`unset` 管理员闸门，`/etc/size` 落盘需管理员 |
| `kernel_v25/src/ata.c` | #123：`ata_read_sectors` DRQ 失败返回 -1，与写路径对称 |

文档/记录（非源码，供合并）：

| 文件 | 改动 |
|---|---|
| `docs/PROGRESS.md` | 追加“二十五、v2.38 P0 4GB 内存支持”设计权衡与已知限制 |
| `docs/history.md` | 追加 6 条 CatOS 易错点 |
| `docs/experience.md` | 追加 4 条 CatOS 经验 |
| `docs/night-P0-report.md` | 本报告（新文件） |
| `docs/night-P0-shots/` | 实机截图 PNG 9 张（新目录） |

> 未改动 `docs/CatOS-内核问题清单.md`（编号续 #128+ 由主代理统一合并）。

## 三、实现要点

### 3.1 内存探测链路排查（除两处截断外无其它截断点）

链路：GRUB multiboot `mbi.mem_lower/mem_upper` → `kernel.c` 透传 →
`hw_mem_boot()` 存 `hw.c`；`paging_init(lo, hi)` 建 PMM。显示侧：
`hwinfo`/`top` 走 `hw_num(HW_MEM_*)`；`mem` 命令直读 `mbi`。

原截断点：
1. `hw.c` `HW_MEM_TOTAL_KB`：`hi` 钳到 `63u<<10`（63MiB）——已去除，
   改为 `1024 + mem_upper_kb + mem_lower/1024`，封顶 `4096*1024` KB。
2. `paging.c` `paging_init`：`top` 钳到 `MEM_TOP=0x4000000`（64MiB）——
   已去除，`top` 跟随 `mem_upper`。

`kernel.c` 在 multiboot 信息缺失时的回退值 `63u<<10` 保留（仅无内存映射
兜底，不是对真实报告的截断）。ring3 ABI 不暴露内存大小，无需改。

### 3.2 分页与分配器（paging.c）

- 沿用既有“内核虚拟 = 物理”恒等映射假设（未做内存动态化重构，符合
  ROADMAP 诚实清单暂缓项）：用 4MiB PSE 监督页恒等映射 `[0, top)`；
  尾部不足一个 4MiB 的块整块映射（沿用旧 16-PSE 行为），分配器只发
  `< mem_top` 的帧，尾块内不存在的 RAM/hole 不会被触达。
- KB→字节用 `uint64_t` 中间量：`0x100000 + (uint64_t)mem_upper_kb*1024`，
  避免近 4GiB 时 `<<10` 在 32 位回绕；超 4GiB 封顶 `0xFFFFF000`（4GiB
  本身不可由 32 位字节地址表示）。
- 位图按全 4GiB 预留：`1MiB 帧 / 32 = 32768` 字（128KiB BSS）；
  `mark_frame` 加帧序号上界检查；`frame_free` 边界改为 `pa >= mem_top`。
- `task_space_create` 由“只拷前 16 个 PDE”改为镜像全部 1024 个内核 PDE；
  高 LFB PT PDE 随 kdir 一起镜像且保持监督态（无 US 位），用户 PDE 8/9
  仍替换为任务私有页表。
- DMA 预留区（PCnet 0x500000..0x510000、SB16 0x510000..0x528000）
  隔离逻辑保持不变。

### 3.3 遗留项处置

- **#123（ata_read_sectors DRQ 失败静默继续）**：确认是真实缺陷。读路径
  原空分支吞掉 `ata_wait_drq()` 错误继续时钟数据口，会把陈旧/垃圾扇区当
  成功交给调用方；改为 `return -1`，与 `ata_write_sectors` 对称。
  已核对全部调用方（fat.c 三处）均检查 `!= 0` 并上抛，改动安全。
- **#122（非 root 可改全局 env / 字号默认值）**：env 表为全局唯一、落盘
  `/etc/env`；`/etc/size` 为全局持久字号。按问题清单建议在落盘路径加
  admin 闸门：
  - `export KEY=...` / `unset KEY`：非 root 直接 `permission denied`
    （无参 `export` 列环境变量为只读，仍放行）。
  - `size`：当前会话仍可临时切字号；仅写 `/etc/size` 要求管理员，非 root
    不持久化。
  - 说明：本内核尚无会话级 env 存储，故未做“会话内可改、仅不落盘”的
    折中；这一产品语义可在后续引入 per-session env 时再调整。

## 四、测试命令与结果

QEMU：`/home/user/bin/qemu-system-i386`（包装 `/home/user/qemu-root/usr/bin`）。
启动参数：`-m <mb> -no-reboot -net none -cdrom catos.iso -boot order=d
-display none -monitor tcp:127.0.0.1:<port> -drive file=catos_disk.img,...`，
纯 PS/2，未加 USB keyboard。

### 4.1 三档内存实机（手动探针 `verify_mem.py` / `probe_mem.py`）

| `-m` | `mem` Upper | `hwinfo` mem.total | PMM free 帧 | hello | allocprobe |
|---|---|---|---|---|---|
| 256 | 260992 KB（255 MB） | 262016 KB | 59619 | exit(0) | PASS |
| 1024 | 1047424 KB（1023 MB） | 1048448 KB | 256227 | exit(0) | PASS |
| 3840 | 3144576 KB（3071 MB） | 3145600 KB | 780515 | exit(0) | PASS |

- gfx24 画面在 `-m 256` 实机渲染正常（四色块 / 渐变条 / 色带齐全），
  证明扩展 PDE 镜像后 LFB 高地址映射仍可用。
- 数值勾稽：total KB ≈ 1024 + upper + lower/1024；free 帧 ×4KiB 与总量
  之差为内核镜像（7155 KB）+ 低 1MiB + DMA 预留，符合预期。

### 4.2 回归脚本

| 脚本 | 命令 | 结果 |
|---|---|---|
| qemu_regress.py | `python3 tests/qemu_regress.py --qemu <qemu> --port 4706` | 11 PASS / 1 WARN（首帧 bright=0.01 临界，非功能失败；final-frame 0.05 PASS） |
| env_regress.py | `python3 tests/env_regress.py kernel_v25/catos.iso --qemu <qemu>` | 13/13 PASS |
| disk_regress.py | `python3 tests/disk_regress.py --qemu <qemu>` | 5/5 PASS（mount/ls-bin/hello/vprobe/fsck，覆盖 ATA 读路径） |
| user_regress.py | `python3 tests/user_regress.py --qemu <qemu>` | 15/15 PASS（既有 ACL 模型未破坏） |
| heap_regress.py | `python3 tests/heap_regress.py --qemu <qemu> --port 4708` | 3/4：allocprobe PASS、heaptest PASS；`ls-bin` FAIL |
| size_regress.py | `python3 tests/size_regress.py kernel_v25/catos.iso --qemu <qemu>` | 见 4.3 既有问题 |

`heap_regress.py` 的 `ls-bin` 断言在 `ls /bin` 后读 80×25 视口文本，39 个
内置应用滚动出屏导致 `allocprobe.ce/heaptest.ce` 不在当前帧；随后两个应用
按路径 `run /bin/...` 均 PASS。该断言是屏幕采集出屏限制，与本次内存改动
无关（本次未改 ls/fat/安装逻辑）。

### 4.3 A/B 基线对照（size_regress 失败非本次回归）

size_regress 的 normal/compact/large-band 与 size-persisted 在**基线源码**
（`git stash` 本次改动后重建 iso）上同样失败：

- 绿带检测常量 `GREEN=(0x55,0xFF,0x55)` 为旧主题值；当前 gfx24 实际强调绿
  为 `#7EE787=(126,231,135)`（见 experience.md 现代主题配色表），像素采样
  基线 ppm 即为此色，精确匹配不到 0x55FF55。
- size_regress 流程未挂 IDE 磁盘镜像，`/etc/size` 无法持久化，persisted
  检查在基线同样 `cat: no such file`。

结论：上述失败为既有脚本/环境问题，不在本分片修复范围（问题清单编号由
主代理续 #128+ 统一登记）。

## 五、截图清单（绝对路径，均已 Read 核对内容）

目录：`/home/user/Doubao/chats/38453665424926722/night/CatOs-P0/docs/night-P0-shots/`

- `m256_mem.png` — `-m 256` mem：255 MB，PMM free 59619 帧
- `m256_hwinfo.png` — `-m 256` hwinfo：mem.total 262016 KB
- `m256_gfx24.png` — gfx24 四色块/渐变/色带渲染正常
- `m1024_mem.png` — `-m 1024` mem：1023 MB，free 256227 帧
- `m1024_hwinfo.png` — `-m 1024` hwinfo：mem.total 1048448 KB
- `m3840_mem.png` — `-m 3840` mem：3071 MB，free 780515 帧
- `m3840_hwinfo.png` — `-m 3840` hwinfo：mem.total 3145600 KB
- `m3840b_mem.png` — 重建后 iso 复跑 `-m 3840` mem 复核
- `m3840b_hwinfo.png` — 重建后 iso 复跑 `-m 3840` hwinfo 复核

## 六、遗留问题与已知限制

1. **本机无法实跑 `-m 4096`**：云端主机 cgroup `memory.max=4GiB`、总内存
   3.9GiB，QEMU 启动 4096MB 客户机即 `Cannot allocate memory`；实测可行
   上限约 3840MiB。代码已按 4GiB 边界做封顶与 64 位换算，但 4GiB 整档
   实机未在本机验证。
2. **PCI hole 之上的第二段 RAM 未枚举**：multiboot `mem_upper` 只统计
   1MiB 以上连续低内存；hole 之上的 RAM 仅出现在 mmap 表，本内核未解析。
   因此大内存客户机显示总量小于 `-m` 标称值（3840MiB 标称 → 报告 ~3071MiB），
   这是如实反映，不是内存泄漏。
3. **真 4GiB 客户机的 MMIO 避让**：当前恒等映射未按 multiboot mmap 避让
   PCI BAR/MMIO 区间；若 BAR 落入最后 4GiB 块，PSE 恒等页会覆盖 MMIO。
   本机测试矩阵 RAM 顶 < BAR0(0xfd000000)，无冲突；彻底解决需后续每帧
   内核窗口方案（ROADMAP 内存动态化 deferred 项）。
4. **size_regress 绿带常量陈旧 / 未挂盘**：见 4.3，建议主代理后续更新
   检测色值或给该脚本挂磁盘镜像。
5. **#122 产品语义**：当前对非 root 直接拒绝 export/unset；若后续引入
   per-session env，可改为“会话内可改、全局落盘才需 admin”。

## 七、改动全部文件列表（供主代理合并冲突预警）

源码：
- `kernel_v25/src/paging.c`
- `kernel_v25/src/hw.c`
- `kernel_v25/src/shell.c`
- `kernel_v25/src/ata.c`

文档：
- `docs/PROGRESS.md`（追加一节）
- `docs/history.md`（追加 6 行）
- `docs/experience.md`（追加 4 行）
- `docs/night-P0-report.md`（新文件）
- `docs/night-P0-shots/`（9 张 PNG，新目录）

未改：`docs/CatOS-内核问题清单.md`、`docs/TODO-2026-11-12-VMware反馈轮.md`、
CE 应用源码、FAT/磁盘镜像构建产物。
