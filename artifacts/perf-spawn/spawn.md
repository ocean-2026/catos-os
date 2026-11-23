# perf(spawn) — 进程创建到退出成本优化报告

分支 `perf/spawn`（基于 `perf/heavy` `fbcd5ff`）。优化线：只动 spawn→exec→exit 回收路径及其在该链路上触发的控制台输出路径。

## 1. 测量口径（与 PERF_SPEC 一致）

- 构建：`cd kernel_v25 && BENCH=1 sh build.sh kernel && BENCH=1 sh build.sh iso`
- 命令：`python3 bench/qrun.py --tag <tag> --cmd "bench task" --disk /home/user/work/catos-perf-heavy/bench/disk-ref-64m.img --repeat 3`
- QEMU：`/usr/bin/qemu-system-i386` 6.2.0，`-m 256 -smp 1 -cpu qemu32 -net none`，mhz 中位数 2337，每次 bench 10 次 spawn/exit，取 3 次中位数。
- 前后两轮为**同一套 BENCH 构建、同一 qrun 命令行**；唯一差异是 `v24_scroll` 的拷贝循环（字节 vs 字宽）。内核内 rdtsc 打点同样只在 `CATOS_BENCH` 下编译，两轮都在，互相抵消。
- 参考盘由 qrun 每次复制私有副本，原始盘未被挂载修改。

## 2. exit 窗口子步骤明细（rdtsc 证据）

`bench task` 的 `cycles_exit` 是父进程 `task_wait_child()` 从阻塞到被唤醒的整段窗口，里面同时包含：子进程 ring3 运行、子进程各 syscall、内核回收、调度器切回父进程。在 `task_exit_current` / `task_cleanup` / `task_space_destroy` / SYS_WRITE / `v24_scroll` 内部按子步骤插了 rdtsc 窗口（`[perf] spawn.exit / spawn.sys / spawn.scroll` 行）。

基线（ab-base，字节循环）30 次退出的每步中位数：

| 子步骤 | 中位 cycles/退出 | 占 exit 窗口 | 说明 |
|---|---:|---:|---|
| **SYS_WRITE 控制台绘制（paint）** | **40.16 M** | ≈100%* | 子进程 6 行 stdout 的 `vga_write_n` 全量 |
| └ 其中 gfx24 LFB 滚动像素搬运 `v24_scroll` | ≈36.2 M | **≈95%** | 7.28 cycles/byte；每次滚动搬 1.3824 MiB |
| └ SYS_WRITE 拷用户串（copy） | 30.2 k | ≈0.08% | `copy_from_user` 分块快照 |
| 内核回收 `task_cleanup` 合计 | 42.8 k | **0.11%** | 见下拆项 |
| └ CR3 切回内核目录 | 8.9 k–32.4 k | — | `cpu_cr3_write(paging_kdir())` |
| └ `task_space_destroy` | 22.4 k | — | 页表/帧回收 |
| &nbsp;&nbsp;&nbsp;· PT 1024 项扫描 + present 帧 free | 8.5 k–15.4 k | — | 35 个 present 帧 |
| &nbsp;&nbsp;&nbsp;· PT2 1024 项扫描 + present 帧 free | 5.5 k–10.2 k | — | |
| &nbsp;&nbsp;&nbsp;· 三张表帧（dir/pt/pt2）free | 小 | — | |
| └ VMA 链表节点 free（3 节点） | 小 | — | `vma_free` |
| └ `pipe_task_cleanup` | 小 | — | 扫 PIPE_MAX 句柄 |
| └ 控制台释放 | 小 | — | |
| SYS_TIME_STR（RTC） | 21.4 k | 0.06% | |
| SYS_ALLOC（堆首页 fault+映射） | 89.4 k | 0.24% | |
| SYS_FREE | 8.6 k | 0.02% | |
| 僵尸 reap + 唤醒父进程（wake） | 3.4 k | 0.01% | |
| 每次退出回收帧数 | **35 帧** | — | `g_sp_frames` |

\* paint 中位数略大于 exit 窗口中位数，是因为各子步骤分别取中位数、不可线性相加；以 scroll 聚合口径为准：基线 30 次退出共 149.3 MiB 滚动字节、1086.5 M cycles，折合 **36.2 M cycles/退出 ≈ exit 窗口的 95%**。

### 结论（最贵子步骤）

原始基线把 exit 记为 85%，容易以为贵在回收。rdtsc 细分后：

- **内核回收路径本身已经很薄**：整段 `task_cleanup` 中位 42.8 k cycles（≈0.018 ms），其中页表/帧回收 22.4 k。`frame_free` 本就是 O(1) 位图清零；退出路径上**没有**对大块缓冲做全量 memset（E3 的 per-tid 静态缓冲约 426 KiB 是 BSS 静态分配，本次未改动其生命周期）。
- **真正占 exit 窗口 ≈95% 的是子进程 stdout 在 gfx24 控制台触发的滚动像素搬运**：`v24_scroll()` 把上移 n 行的 LFB 用**逐字节**循环拷贝。Bochs/std VGA 的 LFB 是 MMIO，TCG 下每次 volatile 字节访问都要退出翻译循环陷到 VGA 模拟；一次滚动要搬 `(37-1)×16×2400 = 1,382,400` 字节，实测 **7.28 cycles/byte**。屏幕滚满后每次退出滚动 5 行，是 exit 窗口的绝对大头。

## 3. 改动

唯一功能优化：`kernel_v25/src/be_vbe24.c` 的 `v24_scroll()`。

- 旧：`for (i = 0; i < keep; i++) lfb[i] = lfb[i + shift];` 逐字节 MMIO 拷贝。
- 新：新增 `lfb_move_up()`，按 **32 位字**拷贝主体，尾部字节收尾。拷贝是**逐字 verbatim 内存搬运**、不做通道重排，因此 3 字节 Bochs BGR 与 4 字节 SVGA BGRX 布局都成立；`shift`/`keep` 均为行 pitch 的整数倍，LFB 基址页对齐，字访问天然对齐；上移滚动 `dest < src`，前向拷贝对重叠区安全。
- 效果：每次 MMIO 陷阱搬 4 字节而非 1 字节。

其余改动全部是 `#ifdef CATOS_BENCH` 下的 rdtsc 诊断打点（`task.c`、`paging.c`、`be_vbe24.c`），非 bench 构建不编译、不影响生产路径；已验证不带 `BENCH=1` 的普通 `build.sh kernel` 编译链接干净。

未改动：`frame_free`（已是 O(1)）、VMA/页表回收结构、任务表摘链方式、E3 静态缓冲生命周期。

## 4. 前后对比（3 次中位数，同一 bench 构建、同一 QEMU 命令行）

| 指标（每退出） | ab-base（字节） | ab-opt（字宽） | 变化 |
|---|---:|---:|---:|
| total cycles | 43.25 M（18.51 ms） | 18.36 M（7.86 ms） | **−57.6%** |
| setup cycles | 5.40 M（2.31 ms） | 5.59 M（2.39 ms） | +3.5%（噪声内，无回退） |
| **exit cycles** | **38.00 M（16.26 ms）** | **12.78 M（5.47 ms）** | **−66.4%** |
| setup 占比 | 12.5% | 30.4% | — |
| exit 占比 | 87.5% | 69.6% | — |
| 滚动速度 | 7.28 cycles/byte | 1.84 cycles/byte | **3.96×** |
| 滚动总 cycles（30 退出） | 1086.5 M | 275.1 M | −74.7% |
| 回收 cleanup 中位 | 42.8 k | 36.9 k | 持平（噪声内） |
| 回收 destroy 中位 | 22.4 k | 19.4 k | 持平（噪声内） |

逐轮原始值（cycles / setup / exit，单位 cycles）：

- ab-base：432,527,472 / 52,533,780 / 379,993,692；414,413,220 / 53,960,218 / 360,453,002；467,453,090 / 62,028,720 / 405,424,370
- ab-opt：183,624,870 / 55,851,016 / 127,773,854；199,679,688 / 58,472,882 / 141,206,806；171,143,544 / 47,316,360 / 123,827,184

## 5. 预期收益 vs 实际收益，差异原因

- **预期**：任务提示的典型方向是回收侧（整片地址空间批量回收、避免逐页 TLB 刷新、frame free O(1) 链式回收、去掉每任务大缓冲 memset、减少任务表扫描）。
- **实测前提不成立**：rdtsc 显示回收侧合计 ≤0.2% 的 exit 窗口——`frame_free` 已是 O(1) 位图操作；destroy 只扫 2×1024 项但 present 帧仅 35 个；退出路径没有全量 memset；任务表仅 16 槽。回收侧没有证据支持的可压榨空间，因此**没有为了凑方向去改回收结构**。
- **实际最贵步骤**：子进程 stdout 触发的 gfx24 LFB 逐字节滚动搬运。
- **预期 vs 实际**：字宽拷贝按陷阱次数预期约 4×；实测 7.28→1.84 cycles/byte = **3.96×**，与预期吻合。exit 窗口 −66.4%（小于滚动本身的 −74.7%），因为 exit 里还剩逐字符字形绘制、RTC/alloc 等未优化项。
- **宿主负载噪声**：3 轮中有个别轮次被宿主 loadavg 抬升（setup/exit 同步抬高），故以中位数为准；背靠背 A/B 两轮的 mhz 一致（2337）。

## 6. 正确性门禁证据

### 6.1 spawn/exit 浸泡 + 帧泄漏（60 次循环）

`python3 artifacts/perf-spawn/spawn_leak_probe.py kernel_v25/catos.iso --n 60`（文本控制台，完整 spawn→ring3→exit→reclaim）：

```
baseline free frames: 55263
after 60 runs free frames: 55263
cycles run        : 60 (prompt failures 0)
free frames base  : 55263
free frames after : 55263
frame delta       : +0
zombie tasks      : 0
shell responsive  : True
GATE: PASS
```

60 次 spawn/exit 后空闲帧回到基线（delta 0），`ps` 无 ZOMBIE，shell 仍响应。

### 6.2 vprobe 回归

`python3 tests/vprobe_regress.py`（最终 ISO）：

```
PASS format / bin-has-vprobe / fr / cat / hw / str / strcpy / wr / fw / pw
steps=10 ok=10 fail=0
```

### 6.3 启动

3 轮 bench 串口均有 `[boot] shell-ready`，无 `PANIC` / `page fault` / `kernel fault` / `system halted`。普通（非 BENCH）内核 `build.sh kernel` 编译链接干净。

## 7. 做不动 / 未做的部分（如实说明）

1. **逐字符字形绘制仍是 MMIO 逐字节写**：优化后 exit 剩余的 ~11.6 M cycles/退出主要在 `draw_cell`（背景 `fill` + 字形 ink 像素）。这属于 gfx24 像素核心，由像素级回归套件守护；本次未动，避免破坏 BGR 通道顺序与既有像素断言。
2. **回收侧微优化未实施**：空 PT 快速跳过、批量 PTE 回收等，实测 <0.2% exit 窗口，没有证据支撑的收益，不改。
3. **gfx24_regress.py 在本环境跑不全**：它格式化自己的 64 MiB 暂存盘，但内置程序播种到 `/appdevelop/API.TXT` 时报 `disk full?`，`/bin/gfx24.ce`（连同 hello.ce 等）未落盘，demo 未绘制，13 项像素断言因画面空白而 FAIL。这是既有的 64M 卷播种容量问题，与本次 scroll 改动无关；本环境无法用该套件验证字宽拷贝的像素正确性。字宽拷贝是纯 verbatim 内存搬运（不解读通道），按构造不改变像素字节；vprobe 10/10 与 60 次浸泡通过。
4. 未使用任何近似指标冒充收益：所有占比与倍数均来自内核 rdtsc 计数或 `mem` 帧计数。

## 8. 复跑命令

```bash
# 构建
cd /home/user/work/catos-perf-spawn/kernel_v25
BENCH=1 sh build.sh kernel && BENCH=1 sh build.sh iso

# 性能测量（3 次中位数）
cd /home/user/work/catos-perf-spawn
python3 bench/qrun.py --tag spawn-ab-opt --cmd "bench task" \
    --disk /home/user/work/catos-perf-heavy/bench/disk-ref-64m.img --repeat 3

# 回归
python3 tests/vprobe_regress.py

# 泄漏浸泡
python3 artifacts/perf-spawn/spawn_leak_probe.py kernel_v25/catos.iso --n 60
```

## 9. 改动文件清单

- `kernel_v25/src/be_vbe24.c`：`lfb_move_up()` 字宽 LFB 搬运 + `v24_scroll()` 接入；BENCH 滚动计时计数。
- `kernel_v25/src/task.c`：BENCH 子步骤 rdtsc 累加器（exit/cleanup/各 syscall/wait），诊断用，不改动回收逻辑。
- `kernel_v25/src/paging.c`：BENCH `task_space_destroy` 分段计时与回收帧数计数，诊断用。
- `artifacts/perf-spawn/spawn_leak_probe.py`：60 次 spawn/exit 泄漏与僵尸门禁脚本。

禁止项均未触碰：`bench/perf.c`、`perf.h`、`qrun.py`、`report.py`、`PERF_SPEC.md`、`bench_heavy.c` 的 gfx/disk/pipe 场景、`src/tls*.c`、`kernel_v25/catos_disk.img`；未跑 `build.sh disk`；未动主树 `/home/user/work/catos-os`；未 push、未发版本号、未在仓库根放 ISO。
