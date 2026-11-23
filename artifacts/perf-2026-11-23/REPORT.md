# CatOS 主体性能：统一基准 + 实测瓶颈优化（2026-11-23）

- 分支：`feat/perf-bench`（骨架）→ `perf/light` / `perf/heavy` / `perf/pipe` / `perf/spawn`（四条独立优化线）→ `perf/integ`（合并）
- 主树基线：`3e6957e`（main：E3 per-tid syscall 缓冲 + F11 `$C$` 加盐 SHA-256 已合并）
- 合并后集成 commit：见 `git log perf/integ`（本报告与 `docs/全局待办总表.md` 同一次提交）
- 本轮口径：**先建统一基准把 9 个轴全部量化，再按实测瓶颈决定优化顺序**。没有数字的"变快了"不进本报告。

## 1. 方法与口径（可复现）

| 项 | 值 |
|---|---|
| 构建 | `cd kernel_v25 && BENCH=1 sh build.sh kernel && BENCH=1 sh build.sh iso`（`BENCH=1` → `-DCATOS_BENCH`） |
| 测跑入口 | `bench/qrun.py`：机器级 `flock`（`/home/user/work/catos-bench.lock`）串行化 + `taskset -c 1` 绑核 + 每 run 复制私有磁盘副本 |
| QEMU | `/usr/bin/qemu-system-i386` 6.2.0（`-m 256 -smp 1 -cpu qemu32 -no-reboot -net none -display none -boot order=d -serial file:<log>`） |
| 采样 | 每配置 **3 次**，取中位数；`mhz=2336~2337`（guest 自校准 TSC） |
| 判据 | 只解析串口 `[perf]` / `[btime]` 行（控制台默认 gfx24，0xB8000 文本缓冲为空，不用抓屏做判定） |
| 单位 | `us = cycles / mhz`；`MB/s = bytes * mhz / cycles`；`fps = mhz*1e6 / cycles_per_frame` |
| 前后可比性 | 前后均为 **BENCH 构建 + 同一 QEMU 命令行**；生产构建（无 `BENCH=1`）不含任何计数器 |
| 代码骨架 | `kernel_v25/src/perf.c` / `perf.h`（rdtsc 行输出）、`bench/run_light.py`、`bench/run_heavy.py`；规格 `bench/PERF_SPEC.md` |

原始日志（不入库）：`/home/user/work/catos-bench-runs/{baseline-light,baseline-heavy-nodisk,baseline-heavy-disk,final-light,final-heavy-nodisk,final-heavy-disk}/`
汇总数据：`artifacts/perf-2026-11-23/final.json`、`baseline-sources.md`；各轴细节见 `artifacts/perf-render/`、`perf-fs/`、`perf-pipe/`、`perf-spawn/`、`perf-baseline/`。

## 2. 九轴前后总表（3 次中位数）

| # | 轴 | 指标（metric） | 优化前 | 优化后 | 变化 | 结论 |
|---|---|---|---|---|---|---|
| 1 | 开机到提示符 | `boot.host`（host，含 GRUB） | 792.3 ms | 727.4 ms | 1.09× | guest 侧 `S9` 350→280 ms；差异在噪声级，未优化 |
| 2 | shell 命令往返·echo | `shell.cmd` kind=1 | 3,083.6 µs | 747.2 µs | **4.13×** | 渲染路径受益 |
| 3 | shell 命令往返·`ls /bin` | `shell.cmd` kind=2 | 147,591.4 µs | 35,824.7 µs | **4.12×** | 渲染路径受益（最大交互延迟项） |
| 4 | shell 命令往返·`edit -c` | `shell.cmd` kind=3 | 10,251.0 µs | 2,284.4 µs | **4.49×** | 渲染 + 读路径共同受益 |
| 5 | syscall·gettid | `syscall.rtt` kind=0 | 1.176 µs | 1.324 µs | 0.89× | **bench 构建打点开销**，生产无此开销 |
| 6 | syscall·write("x") | `syscall.rtt` kind=1 | 51.054 µs | 18.377 µs | **2.78×** | 渲染路径 |
| 7 | syscall·file_read 64 B | `syscall.rtt` kind=2 | 811.779 µs | 14.258 µs | **56.9×** | 路径/簇缓存 |
| 8 | 图形一帧（整屏重绘） | `gfx.frame` | 9.47 ms（105.5 fps） | 3.216 ms（311 fps） | **2.95×** | 字形 2.25×、LFB 冲刷 5.44× |
| 9 | 磁盘顺序读 | `disk.seq` | 14.79 MB/s | 15.35 MB/s | 1.04× | **未优化**（见 §5） |
| 10 | 管道吞吐 | `pipe.tput` | 0.350 MB/s | 23.73 MB/s | **67.7×** | 事件驱动阻塞 |
| 11 | 进程创建→退出 | `task.spawn` | 18.22 ms | 6.33 ms | **2.88×** | exit 段 37.49→11.43 M cycles（3.28×） |
| 12 | 登录口令校验 | `login.verify` | 6.501 µs | 6.460 µs | 1.01× | 未优化（F11 安全成本保留） |

系统调用内核段（`syscall.kernel`）：gettid 0.106→0.200 µs（打点开销）、write 50.037→17.262 µs（2.90×）、file_read 810.157→13.274 µs（61.0×）。
登录：`login.hash` 2.964→2.945 µs（纯 SHA-256 未变）。

> 说明：本表"优化后"取自**合并后集成构建**（含两条线的全部打点）。合并构建每个 syscall 多两次 `rdtsc` + 一次按 syscall 号记账，这正是 gettid（0.2 µs 级）反向变慢、管道 23.7 MB/s 低于单线 34.1 MB/s 的原因；生产构建（`BENCH` 关闭）不存在这些计数器。

## 3. 逐条优化：证据 → 改动 → 预期 vs 实际

### 3.1 gfx24 渲染（`perf/heavy`，报告 `artifacts/perf-render/render.md`）
- **证据**：整屏重绘 22.14 M cycles/帧，其中字形 13.2 M（60%）、LFB 冲刷 8.98 M（41%）；单字符 9,055 cycles。TCG 下每次 volatile MMIO 字节访问都要出翻译器。
- **改动**：`be_vbe24` 的字形/冲刷改为按行/按 32-bit 字批量写，减少 MMIO 访问次数；`v24_scroll` 改为字宽 verbatim 搬运（见 3.2）。未改像素语义（BGR/BGRX 通道顺序无关）。
- **预期 vs 实际**：预期冲刷段 2–4×、字形 1.5–2×；实际冲刷 5.34×、字形 2.33×，整帧 3.08×（单线测量）。**像素一致性**：演示区域 1,152,000 字节逐字节 0 差异，MD5 前后一致；整屏仅光标闪烁行 8 像素差（与基线自比噪声地板相同）。

### 3.2 控制台滚动（`perf/spawn`，报告 `artifacts/perf-spawn/spawn.md`）
- **证据**：`task.spawn` 的 exit 窗口 37.49 M cycles/次，但 rdtsc 子步骤显示内核回收只有 42.8 K cycles（destroy 22.4 K、35 帧），**≈94% 是子进程 stdout 触发的 `v24_scroll` 逐字节搬 1.38 MiB MMIO**。
- **改动**：`lfb_move_up()` 32 位字宽前向拷贝（对齐、重叠安全、与像素布局无关）；回收路径**不改**（证据表明本就极薄）。
- **预期 vs 实际**：预期滚动 3–4× → 实测 7.28→1.84 cycles/byte（3.96×）；spawn 总 18.51→7.86 ms（单线 −57.6%），合并渲染优化后集成构建 18.22→6.33 ms（2.88×）。

### 3.3 文件读取（`perf/light`，报告 `artifacts/perf-fs/fs.md`）
- **证据**：64 B 读 811.779 µs，内核段占 99.8%；分段 resolve 66.2%（路径重解析 + 目录簇重读）、ide 33.4%（数据簇 PIO 重读）、mem 仅 696 cycles → **"每次搬 16 KiB" 的假设被证伪**，拷贝不是瓶颈。
- **改动**：fs.c 单槽 绝对路径→目录项 缓存 + fat.c 单槽只读簇快照；在 create/delete/rewrite/format/mount/umount/切卷处失效。
- **预期 vs 实际**：预期 resolve/ide 各降一个量级；实际 resolve −99.15%、ide −96.39%，内核段 799.80→16.59 µs（单线 48×）；集成构建 811.779→14.258 µs（56.9×）。
- **一致性门禁**：写后读（变短/变长）、删后重建、raw 截断、追加、簇号复用、格式化后读、mount/umount 卷切换共 8 项全 PASS（`fs.md` §8）；vprobe 10/10。

### 3.4 管道吞吐（`perf/pipe`，报告 `artifacts/perf-pipe/pipe.md`）
- **证据**：65,536 B 用 440.44 M cycles（0.350 MB/s），`sleeps=33 ticks=17` → ≈90% 墙钟在 10 ms 定时器粒度的 `task_sleep_ticks(1)` 轮询；FIFO 拷贝仅 2.09 M cycles。
- **改动**：写满/读空改为挂 `T_WAIT_PIPE` 条件阻塞，由对端写入/读出/关端显式唤醒（新增 waiter 槽位），退出时清理等待槽并对端唤醒；网络轮询路径保持原样。
- **预期 vs 实际**：预期去掉 tick 粒度后 10× 以上；单线实测 0.350→34.13 MB/s（97.5×，`ticks=0`），集成构建 23.73 MB/s（67.7×）。字节完整性 `bad=0`、`written==got==65536`；E3 2/2 PASS。

### 3.5 补充：轻轴基线里的 `ls /bin`（147.6 ms）
- 基线里它是最大交互延迟项（占三条 shell 命令 91.7%）。本轮**由渲染优化顺带拿到 4.12×**（147.6→35.8 ms）；其剩余成本仍以字形逐像素 MMIO 为主（见 §5）。

## 4. 回归门禁（生产构建，`sh build.sh iso` 无 BENCH）

| 套件 | 命令 | 结果 |
|---|---|---|
| vprobe（Issue #78） | `python3 tests/vprobe_regress.py` | **10/10 PASS**（`steps=10 ok=10 fail=0`，`gate-vprobe.txt`） |
| E3 并发管道 + 恶意指针 | `python3 tests/e3_regress.py` | **2/2 PASS**（`PASS badptr` / `PASS conc`，`gate-e3.txt`） |
| F11 场景一（`$C$`/权限/掩码） | `python3 artifacts/f11-passwd/s1_format_perms.py` | **9/9 PASS**（`gate-f11-s1.txt`） |

生产构建产物（**合入 main 后的主树重建**，门禁即跑在该 ISO 上）：`kernel_v25/build/kernel.bin` **1,190,596 B**、`kernel_v25/catos.iso` **11,917,312 B**（sha256 前 8 位 `9f25f13b`）。
集成工作树同名产物为 1,190,644 B / `b064376e`：同一 commit 两次构建的 48 B 差异来自符号表生成顺序（`symtab.inc` 由临时代码 PE 的 nm 输出生成），不影响门禁结论——三项门禁在主树产物上重跑，结果一致。
注：生产构建仍编入 `bench_light.c`/`bench_heavy.c` 的场景代码（shell 入口已 `#ifdef CATOS_BENCH` 移除、不可达），相对基线 kernel.bin +48,408 B；若要彻底去掉，可整文件包 `#ifdef CATOS_BENCH`（本轮未做，避免影响已验证的 bench 构建）。

## 5. 未优化 / 做不动的方向（如实）

| 轴 | 实测 | 为什么本轮不动 |
|---|---|---|
| 磁盘顺序读 | 14.79→15.35 MB/s（+3.8%，噪声级） | 分段显示 xfer 64%（PIO `insw` 搬 4 MiB）、wait 35%。要显著改善需要多扇区/`insl`/总线主 DMA，属驱动改造，且 QEMU 下 host I/O 缓存会放大测量波动；本轮未做，无收益就不写成收益 |
| 字形逐像素 MMIO | 优化后仍 5,536 cycles/格 | 墨迹仍是逐像素 `put3`、bold/italic 在像素内层；预展开掩码/行级批量写收益明确但改像素核心，需更强像素回归守护，留待下一轮 |
| 开机 792 ms（host）/ guest 350 ms | 最大段 K4 `cpu_calibrate` 100 ms 属**有意等待**（TSC 精度决定蓝屏/watchdog 换算），`lang_init` ≈120 ms | 已有结论（`docs/boot-timing.md`），不为数字好看而改精度 |
| 登录校验 6.46 µs | 纯 SHA-256 2.945 µs，解析/比较占 54% | F11 安全成本，未动 |
| gettid RTT 1.176→1.324 µs | 仅 bench 构建 | 两条线的 per-syscall rdtsc 记账在同一构建里叠加；生产构建无计数器 |
| shell 层 `A | B` 管道 | 基线（未改源码）与优化版行为一致，均无 SINK 计数行 | 既有问题，与本轮改动无关，留给单独排查 |

## 6. 复现步骤

```sh
cd <worktree>/kernel_v25
BENCH=1 sh build.sh kernel && BENCH=1 sh build.sh iso     # 测量用
sh build.sh iso                                            # 生产用（无打点）

cd <worktree>
python3 bench/qrun.py --tag my-light --cmd "bench light" --repeat 3 \
    --disk /path/to/catos_disk.img_copy
python3 bench/qrun.py --tag my-heavy-nodisk --cmd "bench gfx" --cmd "bench pipe" \
    --end-count 2 --repeat 3
python3 bench/qrun.py --tag my-heavy-disk --cmd "bench disk" --cmd "bench task" \
    --end-count 2 --repeat 3 --disk /path/to/disk_copy
python3 bench/run_light.py --tag my-light --collect-only
```

- `qrun.py` 自带机器级锁与 `taskset -c 1`，**别绕过**；两核宿主上并发 guest 会互相偷 cycle。
- 磁盘场景必须传 `--disk`（qrun 每次复制私有副本）；**绝不要挂 `kernel_v25/catos_disk.img` 原盘**。
- f11 场景一需要一份 `test_disk.img`（64 MiB，FAT32，含 47 个文件）：
  `python3 kernel_v25/build/mkdisk.py -o test_disk.img -m 64`；它**不会**动 `catos_disk.img`。

## 7. 风险与注意事项

1. **fs 单槽缓存**：多文件交替读会互挤（正确性无问题，只是不累积收益）；若将来新增绕开 `fat_write_cluster` 的就地写路径，必须同步加失效钩子。
2. **管道事件驱动**：等待者挂在 `T_WAIT_PIPE`，由对端写入/读出/关端与任务退出唤醒；网络轮询未改。若后续改调度器切片，需重跑 `bench pipe` 与 E3。
3. **像素一致性**：滚动改为字宽 verbatim 拷贝，按构造不改字节；渲染线已给出逐字节 0 差异证据。`tests/gfx24_regress.py` 在本环境仍失败（暂存盘播种不到 `gfx24.ce`，demo 未绘制），两条线独立复现为**既有环境问题**，不能用它做像素门禁（本轮改用 screendump 逐字节比对）。
4. **bench 增益 ≠ 生产增益**：合并 bench 构建的 syscall/管道数字含打点开销，报告已标注；对外结论以"生产构建 + 前后同构建"为准的项（渲染、滚动、读写路径）为准。

## 8. 脚本索引

| 路径 | 用途 |
|---|---|
| `bench/PERF_SPEC.md` | 口径、QEMU 命令行、锁、串口格式、文件归属 |
| `bench/qrun.py` | 唯一测跑入口（锁 + 绑核 + 串口采集 + 私有盘副本） |
| `bench/report.py` | 通用 `[perf]` 行汇总（中位数 + 派生单位） |
| `bench/run_light.py` / `bench/run_heavy.py` | 轻/重轴跑批与采集 |
| `bench/render_bench.py` / `bench/gate_text.py` | 渲染前后对比 / 文本模式门禁 |
| `kernel_v25/src/perf.c` / `perf.h` | rdtsc 打点与 `[perf]` 行输出（`BENCH=1`） |
| `kernel_v25/src/bench_light.c` / `bench_heavy.c` | 9 轴场景实现 |
| `tests/fs_coh_format.py` / `tests/fs_coh_mount.py` | 缓存一致性 / 卷切换门禁 |
| `artifacts/perf-spawn/spawn_leak_probe.py` | 60 次 spawn/exit 泄漏与僵尸门禁 |
