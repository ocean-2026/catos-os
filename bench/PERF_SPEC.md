# CatOS 性能基准约定（PERF_SPEC）

本目录是 CatOS 主体（`kernel_v25`）性能线的唯一基准口径。**任何"变快了"的结论
必须来自这里的数字**，不接受凭感觉的判断。

## 1. 构建

```sh
cd kernel_v25 && BENCH=1 sh build.sh kernel && BENCH=1 sh build.sh iso
```

`BENCH=1` 追加 `-DCATOS_BENCH`：编进 `src/perf.c` + `src/bench_light.c` +
`src/bench_heavy.c`，打开 `bench` shell 命令，并在 shell 就绪时向串口打
`[boot] shell-ready`。生产构建（不带 `BENCH=1`）编同一批文件但 shell 入口被
`#ifdef` 掉，行为与改动前一致。

**前后对比必须同为 bench 构建**：优化前基线 = main + 本分支打点（零业务改动）；
优化后 = 同一打点 + 该条优化。不允许拿"生产构建"和"bench 构建"混比。

## 2. 统一 QEMU 命令行（唯一允许的形态）

```
taskset -c 1 qemu-system-i386 -m 256 -smp 1 -cpu qemu32 -no-reboot -net none \
  -display none -cdrom <work-tree>/kernel_v25/catos.iso -boot order=d \
  [-drive file=<fresh copy of a disk image>,format=raw,if=ide] \
  -serial file:<log> -monitor tcp:127.0.0.1:<port>,server,nowait
```

- 全部测量走 `bench/qrun.py`，它实现上面这条命令、`taskset` 绑核、串口采集、
  按 PID 结束 guest、每 run 复制私有磁盘副本（**绝不允许直接挂 `kernel_v25/catos_disk.img`**）。
- 只用串口判定启动/结果：默认控制台 gfx24，0xB8000 文本缓冲是空的。
- 机器信息（`loadavg`、`nproc`）写入每次 run 的 `runs.json`。

## 3. 并发纪律

宿主 2 核 + TCG，两个 guest 同时跑会互相偷 cycle。所有测跑串行：`qrun.py`
在 `/home/user/work/catos-bench.lock` 上取 `flock` 排他锁。**不要绕过 qrun 直接
起 qemu 做测量**；调试可以，但数字不许进报告。

## 4. 重复与统计

- 每个配置 **3 次**，取中位数（`bench/report.py` 自动算）。
- 记录 `n`、`mhz`（guest 自校准 TSC 频率），以及 host 侧 `boot.host`。
- 回归判定用"中位数差 ≥ 3% 且 3 次方向一致"作为有效变化，否则记为噪声。

## 5. 串口输出格式（perf.c 产生，report.py 解析）

```
[perf] <metric> key=<十进制整数> key=<...>
```
- 单行、只含整数；`>32 bit` 的值写成 `<key>_hi` / `<key>_lo`（内核无 64 位除法，
  report 端拼回）。
- 每个场景至少一条，字段建议：`cycles`（rdtsc 差值）、`mhz`、`bytes`、
  `count`、`frames`。派生单位由 host 算：

| 量 | 公式 |
|---|---|
| us | `cycles / mhz` |
| 吞吐 MB/s | `bytes * mhz / cycles`（十进制 MB） |
| 帧率 fps | `mhz * 1e6 / cycles_per_frame` |
| 单次 syscall ns | `cycles / count / mhz * 1000` |

- 场景结束时 `bench` 会打一条 `[perf] end`，driver 以它为"命令跑完"锚点。

## 6. 覆盖的轴（metric 命名固定，便于前后对比）

| 轴 | metric | 说明 |
|---|---|---|
| 开机到提示符 | `boot.host`（host 观测，含 GRUB） + `boot.stage`（guest `[btime]`） | 同一 run 同时给 |
| shell 命令往返 | `shell.cmd` (`cycles` = 命令分发到下次提示符前，`count`=1) | `echo` / `ls /bin` / `edit -c` 小文件 |
| syscall 往返 | `syscall.rtt` (`cycles`, `count`) | 从 ring3 发起、内核返回的整段；再拆 `syscall.kernel` 仅内核段 |
| 图形一帧 | `gfx.frame` (`cycles`, `frames`) | gfx24 全屏重绘 |
| 磁盘顺序读 | `disk.seq` (`cycles`, `bytes`) | 原始 ATA PIO 顺序读 N MiB |
| 管道吞吐 | `pipe.tput` (`cycles`, `bytes`) | 复用 E3 8192 B 并发管道用例 |
| 进程创建退出 | `task.spawn` (`cycles`, `count`) | spawn → 退出 |
| 登录口令路径 | `login.verify` (`cycles`, `count`) | F11 `$C$` SHA-256 校验（含盐） |

## 7. 文件归属（并行开发不打架）

| 文件 | 归属 |
|---|---|
| `kernel_v25/src/perf.c` / `perf.h` | 已冻结（改它先问集成方） |
| `kernel_v25/src/bench_light.c` | 轻轴：`boot` / `shell` / `syscall` / `login` |
| `kernel_v25/src/bench_heavy.c` | 重轴：`gfx` / `disk` / `pipe` / `task` |
| `bench/qrun.py` / `report.py` | 已冻结 |
| `bench/run_light.py`、`bench/run_*.py` | 各轴自建，可新增，不改别人的 |

## 8. 产物位置

- 原始日志：`/home/user/work/catos-bench-runs/<tag>/*.run*.log`（不入仓）
- 汇总表：`artifacts/perf-<date>/`（入仓：md + csv + 复跑命令）
- 报告里必须写清：commit、ISO 大小/sha256 前缀、QEMU 版本、`-m/-smp/-cpu`、
  重复次数、每条改动的"预期收益 vs 实际收益"。
