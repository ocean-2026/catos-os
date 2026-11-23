# CatOS 轻轴（light）性能基线 — 测量报告

- 分支：`perf/light`（基于 `bc60259`，feat/perf-bench 骨架冻结）
- 基线 tag：`baseline-light`；测跑日期：2026-11-23
- 本轮只做测量，不做优化；所有数字均为 guest 内 rdtsc 实测，未用 host sleep 推算。
- 原始日志：`/home/user/work/catos-bench-runs/baseline-light/`（`baseline-light.run1.log` … `run3.log` + `runs.json`）
- 复跑脚本：`/home/user/work/catos-perf-light/bench/run_light.py`
- 汇总数据：`artifacts/perf-baseline/light.json`、`light.csv`（同目录）

## 1. 环境与口径

| 项 | 值 |
|---|---|
| QEMU | `/usr/bin/qemu-system-i386`，QEMU 6.2.0 (Debian 1:6.2+dfsg-2ubuntu6.31) |
| 命令行 | `taskset -c 1 /usr/bin/qemu-system-i386 -m 256 -smp 1 -cpu qemu32 -no-reboot -net none -display none -cdrom <iso> -boot order=d -serial file:<log> -monitor tcp:127.0.0.1:4601,server,nowait -drive file:<private disk copy>,format=raw,if=ide` |
| 绑核 / 锁 | `taskset -c 1`；`bench/qrun.py` 持有 `/home/user/work/catos-bench.lock` 排他锁 |
| guest TSC 标定 | `mhz=2337`（来自 `[perf]` 行 `mhz=`） |
| 每配置重复次数 n | 3 次跑；表内 cycles 为 3 次中位数；guest 内每命令样本数见 `count` |
| ISO | `kernel_v25/catos.iso`，11,905,024 字节，sha256 前 8 位 `cc83cab5` |
| 参考盘 | `/home/user/work/catos-os/kernel_v25/catos_disk.img`（qrun 每次复制私有副本，原盘未挂载/未修改） |
| host | nproc=2；3 次跑 loadavg 依次 `0.38 0.38 0.27`、`0.41 0.38 0.27`、`0.37 0.37 0.27` |
| 串口判据 | 只解析 `[perf]` / `[btime]` 行；控制台为 gfx24，未用 VGA 抓屏 |

构建（与 PERF_SPEC §5 一致）：

```
cd /home/user/work/catos-perf-light/kernel_v25
BENCH=1 sh build.sh kernel && BENCH=1 sh build.sh iso
```

复跑基线：

```
cd /home/user/work/catos-perf-light
python3 bench/run_light.py --tag baseline-light --repeat 3
# 仅从已有日志重新汇总：
python3 bench/run_light.py --tag baseline-light --collect-only
```

## 2. 总表（cycles 为 3 次中位数；us = cycles / mhz）

### 2.1 开机到 shell 提示符

| metric | 中位数 | 说明 |
|---|---|---|
| `boot.host` | 792.3 ms | host 观测，含 GRUB（`runs.json` `host_boot_ms` 中位数） |
| guest uptime @ shell-ready | 350 ms | `[btime] S9 shell prompt ready` 累计时间 |

### 2.2 单条 shell 命令往返（`shell.cmd` 内部分发段）

内部通过 `shell_ssh_exec_line` 程序化执行，每命令 guest 内 `count=50` 次（含 1 次未计时 warmup）；
`shell.idle` 为主循环钩子实测的「提示符 + 读行」段，仅在交互行上采集（见 §4.2 口径）。

| metric | kind | 命令 | count | cycles(中位) | per-call cycles | per-call us | 3 次波动 |
|---|---|---|---|---|---|---|---|
| shell.cmd | 1 | `echo benchprobe` | 50 | 360,316,528 | 7,206,330.6 | 3083.582 | 13.6% |
| shell.cmd | 2 | `ls /bin` | 50 | 17,246,404,266 | 344,928,085.3 | 147,591.388 | 13.3% |
| shell.cmd | 3 | `edit -c /etc/passwd` | 50 | 1,197,833,650 | 23,956,673.0 | 10,251.037 | 6.3% |
| shell.idle | 0 | `bench light` 交互行（提示符+读行） | 1 | 7,814,309,282 | 7,814,309,282 | 3343.7 ms | 0.7% |
| shell.cmd | 0 | `bench light` 交互行整段分发（= 整组场景） | 1 | 20,301,547,604 | — | — | 11.9% |

三条命令 per-call cycle 占比：echo 1.9% / **ls /bin 91.7%** / edit 6.4%。

### 2.3 syscall 往返开销（ring3 探针 `benchrt.ce`，每类 count=500）

`syscall.rtt` = ring3 发起 → trap → 内核 handler → iret → ring3 的真实往返；
`syscall.kernel` = 仅 `isr_syscall` 进入到返回的内核分发段（task.c 钩子），**不含** trap/iret 与 ring3↔ring0 栈切换。

| metric | kind | 调用 | count | per-call cycles | per-call us | kernel 占 RTT | 3 次波动 |
|---|---|---|---|---|---|---|---|
| syscall.rtt | 0 | `gettid` | 500 | 2,747.5 | 1.176 | — | 3.0% |
| syscall.kernel | 0 | `gettid` | 500 | 247.2 | 0.106 | 9.0% | 26.2% |
| syscall.rtt | 1 | `write("x")` | 500 | 119,314.0 | 51.054 | — | 16.1% |
| syscall.kernel | 1 | `write("x")` | 500 | 116,935.5 | 50.037 | 98.0% | 16.6% |
| syscall.rtt | 2 | `file_read(/etc/passwd, 64B, 0)` | 500 | 1,897,126.7 | 811.779 | — | 7.0% |
| syscall.kernel | 2 | `file_read(/etc/passwd, 64B, 0)` | 500 | 1,893,336.2 | 810.157 | 99.8% | 7.0% |

探针 `spawn_rc=0`（3 次均成功拉起 ring3 任务并读回结果文件）。

### 2.4 登录口令校验路径（F11 `$C$` 加盐 SHA-256，count=50）

| metric | 含义 | per-call cycles | per-call us | 占 verify | 3 次波动 |
|---|---|---|---|---|---|
| login.verify | 完整 `user_check_pw`：盐/摘要 hex 解析 + SHA-256 + 常量比较 | 15,192.3 | 6.501 | 100% | 2.5% |
| login.hash | 裸 `sha256_2(salt[8]‖pw)`，固定盐，不含解析/比较 | 6,927.4 | 2.964 | 45.6% | 3.3% |
| （派生）parse+compare | verify − hash | 8,264.9 | 3.537 | 54.4% | — |

`bad=0`：50 次校验全部通过；口令仅在内存记录中设置（未 `user_save`，不落盘）。

## 3. 开机分段（`[btime]` 累计 ms，10 ms 分辨率）

| 分段 | 累计 ms | 段内增量 ms |
|---|---|---|
| K0 kernel_main enter | 0 | — |
| K3 intr+tasks+sti | 10 | 10 |
| K4 mem+cpu calibrate（含 vga/kbd/paging/intr/标定） | 110 | 100 |
| S1 fs_init done | 110 | 0 |
| **S2 lang_init** | 230 | **120** |
| S3 env_init | 240 | 10 |
| S4 ce_init | 250 | 10 |
| S5 user/acl/session | 270 | 20 |
| B1 builtins done | 320 | 40（B0 280→B1 320） |
| S7 pkg repo seeded | 320 | 0 |
| S8 autostart done | 340 | 20 |
| S9 shell prompt ready | 350 | 10 |

guest 内最大分段：**S2 lang_init ≈ 120 ms**；其次 K4 标定桶 ≈ 100 ms、builtins 安装 40 ms。

## 4. 口径与噪声说明

### 4.1 syscall 测量边界（诚实标注）
- `syscall.kernel` 是 dispatcher 内部段（`g_in_syscall++` 之后到 switch 结束、写 `f->eax` 之前），**不包含** trap 入口存帧、iret 返栈，也不包含 ring3→ring0 特权级/栈切换。
- `syscall.rtt` 是真实 ring3 负载：`extapp/benchrt.c` 经 vsyscall 表发起 `int 0x80`，rdtsc 包住整次调用；其中包含 thunk、trap/iret、用户态拷贝路径。
- gettid 是无用户缓冲的廉价调用，RTT 中 trap/thunk/iret 占 91.0%（2,747.5 中内核仅 247.2 cycles）；其内核段波动 26.2% 来自 IRQ 与两次 rdtsc 包裹噪声（绝对量级仅 247 cycles/次），中位数方向稳定。
- write 的内核段占 RTT 98.0%：单字符 write 的成本几乎全在控制台渲染（gfx24 字形/刷新），不在 trap 层。
- file_read 内核段占 RTT 99.8%：FAT 簇链走读 + `copy_to_user` 占绝对主体。

### 4.2 E3 per-tid 缓冲（BSS）是否进入热路径
- 每任务缓冲：wr 257 + rl 256 + fr 16384 + fw 4096 + pipe 4096 + pipe 4096 + ur 2048 + ip 24 ≈ 27,261 B/任务 × 16 任务 ≈ 426 KiB 静态 BSS，开机常驻。
- 热路径上只做 `e3_wr_buf[t->tid]` 这类按 tid 的静态数组下标取值；**每次调用不 memset、不重分配、不按 tid 清零缓冲**。WRITE 路径唯一的 memcpy 是 `copy_from_user` 把用户串快照进 chunk（本次探针只写 1 字符）。
- 证据对照：gettid（完全不触缓冲）内核段 247 cycles；write（一次 chunk 快照 + 控制台渲染）116.9k cycles；file_read（16 KiB 级 fr_buf + FAT 读）1.893M cycles。缓冲下标本身不在慢路径上，慢的是后续拷贝/渲染/FAT 读。

### 4.3 shell 段口径
- `shell.cmd` 内部数字包含 `shell_ssh_exec_line` 的每次调用开销（env 展开、tee 挂/摘、捕获拷贝），不是裸 builtin 成本；同口径三次跑可比。
- `shell.idle` 只在主交互循环上采集；本次唯一交互行是 `bench light`，其读数吸收了 HMP 注入 scancode 的 host 侧等待（3.34 s 量级），**不是**纯内核读行成本，不能当作 readline 性能依据。
- 未对 alias 回放、pending-guess 重放这两条旁路 `shell_exec` 路径加钩子（最小侵入，见 §6）。

### 4.4 噪声来源
- TCG 软件模拟 + 单 vCPU；定时器/USB/网络轮询等中断会落在测量窗口内。廉价路径（gettid、echo）相对波动最大；已把 guest 内样本提到 shell 50 次、syscall 500 次、login 50 次。
- 仍 >10% 的项：echo 13.6%、ls 13.3%、write RTT/kernel ≈16%、gettid kernel 26.2%；中位数排序在 3 次跑之间一致同向。

## 5. 瓶颈判断（轻轴，按 cycle 证据）

**轻轴上最该先优化的一处：`ls /bin` 命令分发段。**

- 证据：per-call 中位 **344,928,085 cycles（147.6 ms）**，占三条 shell 命令 per-call 总 cycle 的 **91.7%**；是 echo（7.21M）的 47.9 倍、edit（23.96M）的 14.4 倍。
- 次级瓶颈：`file_read` 内核段 **1,893,336 cycles/64 B（810.2 us）**，占其 RTT 99.8%，是 syscall 轴上最大的单笔内核成本；write 的 116.9k cycles/次几乎全在控制台渲染。
- 开机侧：guest 内最大分段是 **lang_init ≈ 120 ms**（110→230 ms），其次 K4 标定桶 ≈100 ms；host 开机 792.3 ms 中 GRUB/引导占用未在 guest 分段内拆分。
- 登录校验：verify 15.19k cycles 中 45.6% 是纯 SHA-256，54.4% 是 hex 解析 + 常量比较；本轮无内核侧过热证据。

## 6. 改动清单（逐行）

### 6.1 `kernel_v25/src/bench_light.c`（自有文件，由 stub 重写）
- `bench_shell_one()`：warmup 1 次未计时执行，其后 `N_SHELL=50` 次 `shell_ssh_exec_line` 包 rdtsc，合计 cycles 输出 `shell.cmd`（kind 1/2/3，count=50）。
- `bench_syscall()`：置位 task.c 采集开关 → `task_spawn_ce("/bin/benchrt.ce", 0, NULL, UID_ROOT)` → `task_wait_child` → 关开关 → `fs_read_text("/tmp/benchrt.txt")` 解析 `gt/wt/rd/n`，输出 `syscall.rtt` 三类；转发 task.c 计数输出 `syscall.kernel` 三类；`spawn_rc` 记录拉起结果。
- `bench_login()`：`user_set_pw("root", "BenchPass123")` 仅改内存（不 `user_save`）；50 次 `user_check_pw` → `login.verify`；50 次固定盐 `sha256_2` → `login.hash`；`bad` 标记校验是否通过。
- 子命令分发：`bench light`/`shell`/`syscall`/`login`。

### 6.2 `kernel_v25/extapp/benchrt.c`（新增，ring3 探针）
- `N=500`；三类循环内 `rdtsc` 包裹 `gettid` / `write("x")` / `file_read("/etc/passwd", buf, 64, 0)`；结果以 `gt= wt= rd= n=` 单行写入 `/tmp/benchrt.txt`（`file_write`）。freestanding，无 libc，32 位累加。

### 6.3 `kernel_v25/build/apps.list`
- 末尾追加一行 `benchrt`（仅此一行；使新探针被编译并作为 `/bin/benchrt.ce` 内置进 ISO）。

### 6.4 `kernel_v25/src/shell.c`（仅 `#ifdef CATOS_BENCH` 内，最小侵入）
1. 主循环 `prompt()` 之前：`_bidle = perf_now()`。
2. 读行完成、切出 `cmd` 之后：按 `echo/ls/edit` 分类 `_bkind`，输出 `shell.idle`（cycles = `perf_now() - _bidle`）。
3. 主分发 `shell_exec(cmd, arg)` 前后：`_bcmd = perf_now()` 起表，返回后输出 `shell.cmd`（cycles，kind，count=1）。
- 生产构建（无 `CATOS_BENCH`）这些代码不编入；未改动任何既有生产逻辑。

### 6.5 `kernel_v25/src/task.c`（仅 `#ifdef CATOS_BENCH` 内）
1. 顶部 `#ifdef CATOS_BENCH` 引入 `perf.h`。
2. E3 缓冲后新增探针全局：`perf_sc_collect`、`perf_sc_kern[3]`、`perf_sc_count[3]`。
3. `isr_syscall` 中 `g_in_syscall++` 后打入口时间戳 `_sc_ent`。
4. switch 结束、`f->eax = ret` 前：采集开关打开且 nr ∈ {34 GETTID, 0 WRITE, 39 FILE_READ} 时，把 `perf_now() - _sc_ent` 累加到对应桶并计数。

### 6.6 新增脚本 / 产物
- `bench/run_light.py`：调 `qrun.run_batch` 跑锁内绑核批次；按 metric+kind 聚合 3 次中位数，派生 us / per-call / 波动；采集 ISO 字节数与 sha256 前 8 位、nproc、loadavg、QEMU 版本；输出 `light.json` / `light.csv`。`--collect-only` 可只汇总已有日志。
- `artifacts/perf-baseline/light.md`（本文件）、`light.json`、`light.csv`。

## 7. 本轮未做 / 做不到的事（如实记录）

- 未做任何优化；仅建立基线。
- 未测量 ring3→ring0 特权级切换/栈切换本身的独立成本：`syscall.kernel` 明确不含 trap/iret，`syscall.rtt` 已包含整段往返，二者差值即 thunk+trap+iret 段，但未再拆分 thunk 与栈切换。
- `shell.idle` 受 host 注入等待污染，未得到纯内核 readline 周期。
- 未跑 `build.sh disk`；未触碰主树 `/home/user/work/catos-os`；未 push。
- 参考盘 `/home/user/work/catos-os/kernel_v25/catos_disk.img` 只读引用，qrun 私有副本在 `/tmp/catos_bench_<tag>_<rand>/disk.img`。
