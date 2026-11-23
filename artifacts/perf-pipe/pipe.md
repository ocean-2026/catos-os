# perf(pipe): 管道吞吐事件驱动优化报告

## 1. 结论（一句话）

把管道阻塞侧从 100 Hz 定时器 `task_sleep_ticks(1)` 轮询改为按对端条件显式唤醒后，
`pipe.tput` 中位数吞吐从 **0.35 MB/s 提升到 34.13 MB/s（97.5×）**，cycles 中位数
437.62M → 4.49M（97.5×），字节完整性 `bad=0`、`written==got==65536`，E3 回归 2/2 PASS，
启动正常。

- 分支：`perf/pipe`（基于 `perf/heavy` 的 `fbcd5ff`）；本次提交哈希见 `git log -1`
- 工作树：`/home/user/work/catos-perf-pipe`
- 改动文件：`kernel_v25/src/task.c`、`kernel_v25/include/task.h`、`kernel_v25/src/bench_heavy.c`（仅 pipe 场景打点）

## 2. 环境与构建口径

| 项 | 值 |
|---|---|
| QEMU | `/usr/bin/qemu-system-i386` 6.2.0 (Debian 1:6.2+dfsg-2ubuntu6.31) |
| 命令行 | `-m 256 -smp 1 -cpu qemu32 -no-reboot -net none -display none`，`taskset -c 1`，串口落盘（`bench/qrun.py` 固定，未绕过） |
| 构建 | `BENCH=1 sh build.sh kernel` + `BENCH=1 sh build.sh iso`（前后同为 bench 构建） |
| 重复次数 | 每配置 3 次，取中位数（`bench/report.py` 口径） |
| 最终 ISO | `kernel_v25/catos.iso`，11,905,024 字节，sha256 前缀 `aff692f8375d772d` |
| guest TSC 频率 | mhz=2337（两组测量一致；baseline boot 行 2336/2337） |

复跑命令：

```sh
cd /home/user/work/catos-perf-pipe/kernel_v25
BENCH=1 sh build.sh kernel && BENCH=1 sh build.sh iso

# 基线（轮询版，同打点零业务改动）与优化版各 3 次
python3 bench/qrun.py --tag base-pipe  --cmd "bench pipe" --repeat 3
python3 bench/qrun.py --tag opt-pipe2  --cmd "bench pipe" --repeat 3
python3 bench/report.py --tag base-pipe --tag opt-pipe2

# E3 回归门禁
python3 tests/e3_regress.py
```

## 3. 假设与基线复核

**原始判断（任务书给定，先自行复核）**：FIFO 拷贝本身不贵，瓶颈是 100 Hz 定时器粒度
（1 tick = 10 ms）的 `task_sleep_ticks(1)` 轮询，约 90% 墙钟花在阻塞睡眠里。

复核方式：在 FIFO 助手内用 rdtsc 加两段累计计数器（见 §4），基线轮询版先跑 3 次：

- 拷贝段 `cp` 中位数 **2.09M cycles**，仅占 reader 侧总 cycles 的 **0.48%**；
- 等待段 `wt`（双侧聚合）中位数 **844.70M cycles**；
- 场景计数 `sleeps=33`（阻塞事件 33 次）、`ticks=17`（跨越 17 个定时器 tick ≈ 170 ms）；
- 总 cycles 中位数 **437.62M** ≈ 187.25 ms。

结论：判断成立——拷贝不是瓶颈，阻塞等待/唤醒调度才是。轮询被定时器周期粒度钉住：
每次条件不满足都要等到下一个 tick 才被 `task_tick_wake()` 扫回 `T_READY`。

## 4. 分段打点口径（读数字前必读）

打点全部在内核 FIFO 助手里完成，`bench_pipe` 场景在前后快照差值后通过串口上报：

| 串口字段 | 含义 |
|---|---|
| `cycles` | reader 侧（bench 任务）从 spawn writer 到收满 64 KiB 的 rdtsc 总差 |
| `wt` (`wt_hi/wt_lo`) | 所有阻塞点从 `task_block()` 前 rdtsc 到被唤醒恢复后的 rdtsc 差值累计——**包含停车等待 + 对端调度切片 + 上下文切换/唤醒派发** |
| `cp` (`cp_hi/cp_lo`) | FIFO 字节拷贝循环内的 rdtsc 差值累计 |
| `sleeps` | 阻塞事件次数（语义沿用旧 `g_pipe_sleeps`，优化后 = 事件驱动阻塞次数） |
| `ticks` | 场景期间跨越的定时器 tick 数（轮询粒度的直接证据） |

**口径限制（不可把两段相加当总时长）**：`wt`/`cp` 是 **writer + reader 双侧聚合**，
两侧的等待区间在墙钟上互相重叠，因此 `wt + cp` 可能大于 reader 侧 `cycles`；
它们用于前后趋势对比，不是 reader 侧总时长的可加分段。

**三段拆分的诚实说明**：要求的"睡眠/等待 vs 拷贝 vs 唤醒调度"中，"拷贝"有独立计数器；
"停车等待"与"唤醒调度派发"在被唤醒方视角不可再分——被唤醒任务观察不到对端何时把自己
置回 `T_READY`，需要跨任务时间戳相关，TCG 下 rdtsc 跨任务读数不可靠。`wt` 合并覆盖这两段，
报告不用近似指标把它拆成两个假数字。

## 5. 改动内容

### 5.1 调度状态与等待槽（`task.h` / `task.c`）

- 新增任务状态 `T_WAIT_PIPE`：管道阻塞态，与 `T_WAIT_KB` / `T_WAIT_CHILD` 同列。
- `pipe_t` 新增 `wait_r`（等数据的读者）、`wait_w`（等空间的写者）两个等待槽。
- 两个阻塞助手：
  - `pipe_wait_writer(pp)`：登记 `pp->wait_w = cur_task`，`task_block(T_WAIT_PIPE)`，
    恢复后累计 `wt` 并清槽；
  - `pipe_wait_reader(pp)`：同上，登记 `wait_r`。
- 两个唤醒助手：
  - `pipe_wake_reader(pp)`：把等待读者翻回 `T_READY`；
  - `pipe_wake_writer(pp)`：把等待写者翻回 `T_READY`。
- 唤醒时机：写者每批拷入字节后唤醒读者；读者每批排空字节后唤醒写者；
  `close_write`（EOF）唤醒读者，`close_read`（读者已走）唤醒写者；
  `SYS_PIPE_CLOSE` 与任务退出清理 `pipe_task_cleanup` 同样唤醒对端并清等待槽。

### 5.2 覆盖的管道路径（同一套助手，五处阻塞点）

1. `kpipe_write` / `kpipe_read`（ring0 bench 助手，本场景直接走这里）；
2. `SYS_PIPE_WRITE` / `SYS_PIPE_READ`（E3 conc 用例，ring3）；
3. `SYS_WRITE` 的 `out_pipe` 重定向（shell 管道生产者侧）；
4. `SYS_READLINE` 的 `in_pipe` 重定向（shell 管道消费者侧）。

### 5.3 未改动

- 定时器轮询仍保留给非管道路径（`SYS_DELAY`、`SYS_NET_UDP_RECV` 超时轮询等）；
- FIFO 布局、字节顺序、E3 per-tid 暂存缓冲、用户指针校验逻辑均未动；
- 冻结文件（`perf.c`/`perf.h`、`qrun.py`、`report.py`、`PERF_SPEC.md`、
  `bench_heavy.c` 的 gfx/disk/task 场景、`tls*.c`、`catos_disk.img`）未改。

## 6. 预期 vs 实际

**预期（动手前写下）**：拷贝段工作量不变，`cp` 中位数应与基线持平（~2M cycles，噪声内波动）；
等待段去掉 10 ms 粒度轮询后，残留成本只剩 33 次阻塞-唤醒往返的上下文切换/调度切片；
总 cycles 预计落在 **5–30M**（约 2–13 ms，吞吐 5–25 MB/s）。

**实际（最终批次 `opt-pipe2`，n=3，mhz=2337）**：

| 指标 | 基线中位数 | 优化后中位数 | 变化 |
|---|---:|---:|---:|
| `cycles` | 437,612,786 | 4,487,756 | **97.5× 下降** |
| `us`（cycles/mhz） | 187,254.1（187.25 ms） | 1,920.3（1.92 ms） | 97.5× 下降 |
| 吞吐 MB/s（bytes·mhz/cycles） | 0.350 | 34.13 | **97.5× 提升** |
| `wt`（等待+唤醒调度，双侧聚合） | 844,695,280 | 4,058,600 | **208× 下降** |
| `cp`（FIFO 拷贝，双侧聚合） | 2,091,154 | 1,877,434 | 0.90×（噪声内，拷贝工作量未变） |
| `ticks`（定时器轮询粒度证据） | 17 | 0 | 不再依赖定时器兜底 |
| `sleeps`（阻塞事件次数） | 33 | 33 | 事件数不变，机制改变 |

对照预期：cycles 4.49M 略优于预期区间下限，吞吐 34.13 MB/s 高于预期区间上限；
`cp` 与基线持平，证实瓶颈确实不在拷贝。`wt` 绝对值降 208×，残留 4.06M cycles /
33 次往返 ≈ 123K cycles/次，即 TCG 下一次阻塞-唤醒的上下文切换与对端调度切片成本。

### 6.1 每跑明细

基线 `base-pipe`（mhz=2337，bytes=65536，written=65536，chunks=256，bad=0）：

| run | cycles | wt | cp | sleeps | ticks |
|---|---:|---:|---:|---:|---:|
| 1 | 436,943,390 | 843,281,634 | 1,957,956 | 33 | 17 |
| 2 | 437,612,786 | 844,695,280 | 2,209,636 | 33 | 17 |
| 3 | 437,846,656 | 844,913,342 | 2,091,154 | 33 | 17 |

优化后 `opt-pipe2`（mhz=2337，bytes=65536，written=65536，chunks=256，bad=0）：

| run | cycles | wt | cp | sleeps | ticks |
|---|---:|---:|---:|---:|---:|
| 1 | 4,487,756 | 4,039,230 | 1,877,434 | 33 | 0 |
| 2 | 4,483,206 | 4,058,600 | 1,862,276 | 33 | 0 |
| 3 | 4,595,474 | 4,182,750 | 1,887,106 | 33 | 0 |

补充：优化后首次试跑批次 `opt-pipe`（当时宿主另有并行 guest）中位 9.87M cycles、
15.51 MB/s，离散度大（run1 `cp` 7.66M 为离群）；报告结论以宿主空闲后的 `opt-pipe2` 为准。

## 7. 调度公平性与不死锁论证

**使用的队列 / wakeup 机制**：复用内核现有全局任务表与 round-robin 就绪队列，没有另起
私有等待队列。

- 阻塞：`pipe_wait_*` → `task_block(T_WAIT_PIPE)` → `reschedule()` → `pick_ready()`
  只扫描 `T_READY` 任务，从当前 tid 轮转取下一个；被阻塞者不在就绪集合里，**不持锁、不自旋**，
  其它 READY 任务（包括 shell、其它 CE 任务）照常被调度。
- 唤醒：对端写完/读完/关端只做一次状态翻转（`T_WAIT_PIPE` → `T_READY`），**不做直接
  handoff**；对端继续自己的运行，直到它自己阻塞、让出或退出，调度器再按轮转选 READY。
- 定时器 IRQ 的 `task_tick_wake()` 只扫 `T_SLEEPING`，不碰 `T_WAIT_PIPE`；阻塞完全由对端
  事件驱动，`ticks=0` 即为无定时器兜底的证据。
- 不死锁：写者只在管道**满**时阻塞，读者只在管道**空**时阻塞，二者不可能同时等对方；
  任一端关闭都会唤醒另一端（EOF / broken pipe），退出清理路径同样唤醒对端。

**旁证**：E3 conc 用例里父任务在写者满管阻塞期间仍穿插发送 `E3PROBE busy` 控制台流量，
并通过逐字节校验——阻塞期间第二个任务持续取得 CPU。

## 8. 正确性门禁记录

### 8.1 字节完整性（`bench pipe`，优化后 3 次）

三条串口行（`[perf] pipe.tput`）：

```
[perf] pipe.tput cycles_hi=0 cycles_lo=4487756 bytes=65536 written=65536 chunks=256 bad=0 sleeps=33 ticks=0 wt_hi=0 wt_lo=4039230 cp_hi=0 cp_lo=1877434 mhz=2337
[perf] pipe.tput cycles_hi=0 cycles_lo=4483206 bytes=65536 written=65536 chunks=256 bad=0 sleeps=33 ticks=0 wt_hi=0 wt_lo=4058600 cp_hi=0 cp_lo=1862276 mhz=2337
[perf] pipe.tput cycles_hi=0 cycles_lo=4595474 bytes=65536 written=65536 chunks=256 bad=0 sleeps=33 ticks=0 wt_hi=0 wt_lo=4182750 cp_hi=0 cp_lo=1887106 mhz=2337
```

`bad=0`，`bytes==written==65536`，`chunks=256`，三次一致。

### 8.2 E3 回归（`python3 tests/e3_regress.py`，最终 ISO）

```
PASS badptr       verdict PASS
PASS conc         verdict PASS

steps=2 ok=2 fail=0
```

### 8.3 启动健康

优化后 3 次 run 串口各有 1 条 `[boot] shell-ready`，无 `PANIC` / `system halted` /
`kernel fault`；基线 3 次同样干净。

## 9. 局限与做不动的地方（如实记录）

1. **停车等待 vs 唤醒派发未再拆**：见 §4，被唤醒方 TSC 观察不到对端置位 READY 的时刻，
   未用近似指标冒充第三段数字。
2. **shell 层 `A | B` 管道未被自动化门禁覆盖**：本树基线（未改动源码）与优化版在同一
   冒烟步骤下行为一致——`run /bin/hello.ce | /bin/sink_ce.ce` 都没有出现 `SINK got`
   计数行，说明这是**既有问题**，不在本次优化线范围内；本次仅保证 shell 重定向路径
   （`out_pipe`/`in_pipe`）共用同一套事件驱动助手，阻塞/唤醒逻辑与已通过 E3 的
   `SYS_PIPE_*` 路径对称。该既有管道问题留待单独排查。
3. **双侧聚合计数器不可加**：`wt`/`cp` 跨 writer+reader 累计，等待区间在墙钟上重叠，
   不能与 reader 侧 `cycles` 做加减对账。
4. **TCG 抖动**：首次优化批次受并行 guest 干扰离散度较大；最终结论基于宿主空闲后的
   `opt-pipe2` 三次中位。
5. 基线 ISO 已被优化版构建覆盖，未单独留存基线镜像哈希；基线与优化版为同一 bench 配置、
   同一 QEMU 命令行、同一套打点，差异仅在阻塞机制（轮询 vs 事件驱动）。
