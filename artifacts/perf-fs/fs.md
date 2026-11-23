# CatOS 文件读取 / shell 交互路径优化 — 性能报告

- 分支：`perf/light`；工作树 `/home/user/work/catos-perf-light`
- 优化前 tag：`fs-before`；优化后 tag：`fs-after`（perf）、`fs-check`（字节一致性）
- 测跑日期：2026-11-23；本轮只动文件读取路径，**未改渲染 / 字形 / 冲刷代码**。
- 原始日志：`/home/user/work/catos-bench-runs/fs-before/`、`/home/user/work/catos-bench-runs/fs-after/`、`/home/user/work/catos-bench-runs/fs-check/`

## 1. 环境与口径

| 项 | 值 |
|---|---|
| QEMU | `/usr/bin/qemu-system-i386`，QEMU 6.2.0 (Debian 1:6.2+dfsg-2ubuntu6.31) |
| 命令行 | `taskset -c 1 /usr/bin/qemu-system-i386 -m 256 -smp 1 -cpu qemu32 -no-reboot -net none -display none -cdrom <iso> -boot order=d -serial file:<log> -monitor tcp:127.0.0.1:<port>,server,nowait -drive file:<private disk copy>,format=raw,if=ide` |
| 绑核 / 锁 | `taskset -c 1`；`bench/qrun.py` 持有 `/home/user/work/catos-bench.lock` 排他锁 |
| guest TSC 标定 | `mhz=2337`（`[perf]` 行 `mhz=`） |
| 每配置重复 | 3 次跑；表内 cycles 为 3 次中位数；syscall 探针每类 count=500，shell 命令 count=50 |
| 优化前 ISO | `kernel_v25/catos.iso`，11,905,024 字节（`fs-before` runs.json `iso_bytes`） |
| 优化后 ISO | `kernel_v25/catos.iso`，11,909,120 字节，sha256 前 8 位 `e2aca500` |
| 参考盘 | `/home/user/work/catos-os/kernel_v25/catos_disk.img`（67,108,864 B，未修改；qrun 每次复制私有副本） |
| host | nproc=2；`fs-after` 三次跑 loadavg 依次 `0.80 0.82 0.64`、`0.91 0.84 0.65`、`1.31 0.93 0.68`；`fs-before` 依次 `1.46 0.72 0.50`、`1.39 0.73 0.51`、`1.31 0.74 0.52` |
| host 开机观测 | `fs-after` host_boot_ms 837.7 / 791.7 / 1043.7；`fs-before` 777.1 / 780.9 / 726.4 |

构建（与 PERF_SPEC §5 一致；未跑 `build.sh disk`）：

```
cd /home/user/work/catos-perf-light/kernel_v25
BENCH=1 sh build.sh kernel && BENCH=1 sh build.sh iso
```

复跑：

```
cd /home/user/work/catos-perf-light
python3 bench/qrun.py --tag fs-after  --cmd "bench light"   --disk /home/user/work/catos-os/kernel_v25/catos_disk.img --repeat 3
python3 bench/qrun.py --tag fs-check  --cmd "bench fscheck" --disk /home/user/work/catos-os/kernel_v25/catos_disk.img --repeat 3
python3 tests/vprobe_regress.py
```

## 2. 基线回顾（来自 `artifacts/perf-baseline/light.md`，干净 HEAD `03a00ba` 构建）

| 指标 | per-call cycles | per-call us |
|---|---|---|
| `gettid` RTT | 2,747.5 | 1.176 |
| `file_read(/etc/passwd, 64B, 0)` RTT | 1,897,126.7 | 811.779 |
| `file_read` 内核段 | 1,893,336.2 | 810.157（占 RTT 99.8%） |
| `edit -c /etc/passwd` | 23,956,673.0 | 10,251.037 |
| `ls /bin` | 344,928,085.3 | 147,594.388 |

为保证测量口径一致，优化前后对比使用**同一套 `CATOS_BENCH` 打点骨架**下的 `fs-before`（未优化代码）与 `fs-after`（优化后代码）两组跑；干净 HEAD 数字仅作参照。

## 3. 分段打点定位（优化前，`fs-before`，每 call 中位数，count=500）

打点位置：`task.c` `SYS_FILE_READ` 包 `pre`（`u2k_str` 串转换 + `validate_uaddr` + 权限校验）与 `copyout`（`copy_to_user` 回拷）；`fs.c` `fs_read_at` 内分 `resolve`（路径解析 + FAT 目录项查找，含目录簇 IDE 读）、`chain`（内存中 FAT 链跳走）、`ide`（数据簇扇区 PIO 读）、`mem`（簇暂存缓冲 → 调用方逐字节拷贝）。

| 段 | per-call cycles | 占内核段 |
|---|---|---|
| pre（入口校验 / 用户串转换） | 2,705.0 | 0.14% |
| **resolve（路径解析 + 目录项查找）** | **1,238,079.8** | **66.24%** |
| chain（FAT 链内存走读） | 380.3 | 0.02% |
| **ide（数据簇 IDE PIO 读）** | **624,243.1** | **33.40%** |
| mem（暂存缓冲 → 用户拷贝） | 695.9 | 0.04% |
| copyout（`copy_to_user`） | 1,734.9 | 0.09% |
| 未计入（两次 rdtsc 包裹等） | 1,200.7 | 0.06% |
| **内核段合计** | **1,869,139.7** | 100% |
| RTT（ring3 往返） | 1,872,726.9 | — |

**结论（按实测，不凭猜）：**

1. 最贵段是 **resolve（66.2%）**：每次读都从头 `abs_path` 解析、重读根目录簇与 `/etc` 目录簇做目录项扫描，IDE PIO 重读占主体。
2. 其次是 **ide（33.4%）**：数据簇每次都从磁盘 PIO 读一遍。
3. **"每次按 16 KiB 整块全量搬" 的假设被证伪**：`mem` 段仅 696 cycles/call，拷贝循环只按 `take`/`n` 字节走；E3 per-tid 缓冲 `e3_fr_buf[16384]` 只作快照槽位，热路径不 memset、不整块搬。因此本轮**没有**改动拷贝长度逻辑。

## 4. 改动清单

### 4.1 `kernel_v25/src/fs.c` — 单槽绝对路径 → 目录项缓存

- 新增静态槽位 `rc_path[FS_PATH_MAX] / rc_clus / rc_size / rc_valid`。
- `fs_resolve_file`：`abs_path` 规范化后，若槽位命中（路径串相等）直接返回缓存的 cluster + size，跳过父目录扫描与叶子目录项查找；未命中则在 `fat_lookup_name` 成功后回填槽位。
- 失效点（七个写/删入口 + 三个卷结构变更入口）：
  - `create_path`（新建文件/目录）、`fs_save`（落盘，size/cluster 可能变）、`fs_write_text`、`fs_write_raw`、`fs_write_raw_mode`、`fs_rmdir_impl`、`fs_delete`；
  - `fs_format`（卷被清空）、`fs_mount`、`fs_umount`（挂载表变化，缓存目录项可能被遮蔽）。
- 缓存只存 cluster + size，不存文件内容快照。

### 4.2 `kernel_v25/src/fat.c` — 单槽数据簇快照缓存

- 新增静态槽位 `cc_clus / cc_buf[FAT_CLUSTER_SZ_MAX] / cc_valid`（BSS，全任务共享只读快照）。
- `fat_read_cluster`：簇号命中则从快照 `memcpy` 返回，跳过 IDE PIO；未命中先 `ata_read` 进快照槽再拷出。
- 失效点：`fat_write_cluster`（任何簇写）、`fat_activate`（切卷）、`fat_mkfs`（格式化旧卷）。

### 4.3 仅 `CATOS_BENCH` 编译期存在的打点（生产构建不编入）

- `task.c`：`perf_fr_pre`、`perf_fr_copyout` 边界计数。
- `fs.c`：`perf_fs_resolve / chain / ide / mem` 与 `perf_fs_count`。
- `bench_light.c`：`fs.read` 六段转发；`bench fscheck` 子命令（FNV-1a 32 位摘要 + 冷/热双读比对）。

### 4.4 明确未改动

- 16 KiB 暂存缓冲与逐字节拷贝长度逻辑（`mem` 段实测极小，见 §3）。
- 渲染 / 字形 / 冲刷 / 控制台显示路径（归另一轴）。
- 禁改文件：`bench/perf.c`、`perf.h`、`bench/qrun.py`、`bench/report.py`、`PERF_SPEC.md`、`src/tls.c`、`src/tls_aes.c`、`catos_disk.img`；未跑 `build.sh disk`；未动主树 `/home/user/work/catos-os`。

## 5. 优化前后对比（3 次中位数，同一 QEMU 命令行、同一 bench 构建口径）

### 5.1 `file_read(/etc/passwd, 64B, 0)`，count=500

| 指标 | before（fs-before） | after（fs-after） | 变化 |
|---|---|---|---|
| RTT cycles/call | 1,872,726.9 | 40,919.4 | **45.77× 提速，降 97.81%** |
| RTT us/call（÷2337） | 801.34 | 17.51 | — |
| 内核段 cycles/call | 1,869,139.7 | 38,761.9 | **48.22× 提速，降 97.93%** |
| 内核段 us/call | 799.80 | 16.59 | — |

参照干净 HEAD：RTT 1,897,126.7 cycles / 811.779 µs，内核段 1,893,336.2 / 810.157 µs。

三次跑 per-call cycles（RTT / 内核段）：

| run | RTT before | kernel before | RTT after | kernel after |
|---|---|---|---|---|
| 1 | 1,899,364 | 1,894,546 | 41,359 | 39,157 |
| 2 | 1,855,097 | 1,851,169 | 33,883 | 31,354 |
| 3 | 1,872,727 | 1,869,140 | 40,919 | 38,762 |

### 5.2 分段前后对比（per-call cycles / 占内核段）

| 段 | before cycles | before 占比 | after cycles | after 占比 | 段降幅 |
|---|---|---|---|---|---|
| pre | 2,705.0 | 0.14% | 2,516.3 | 6.49% | 入口校验，不在本轴，基本持平 |
| resolve | 1,238,079.8 | 66.24% | 10,486.2 | 27.05% | **−99.15%** |
| chain | 380.3 | 0.02% | 211.7 | 0.55% | −44.3%（内存走读，本就极小） |
| ide | 624,243.1 | 33.40% | 22,561.3 | 58.20% | **−96.39%** |
| mem | 695.9 | 0.04% | 535.3 | 1.38% | −23.2%（拷贝量未改，波动） |
| copyout | 1,734.9 | 0.09% | 1,401.2 | 3.61% | `copy_to_user`，不在本轴，基本持平 |

剩余 after 内核段以 ide（快照命中后的簇拷贝 + 少量未命中）与 resolve（命中后 `abs_path` 规范化 + 路径串比较）为主；pre/copyout 是 trap 边界与用户态拷贝，未在本轮范围内。

### 5.3 shell 命令往返（instrumented 口径 before → after，count=50）

| kind | 命令 | before per-call | after per-call | 变化 |
|---|---|---|---|---|
| 1 | `echo benchprobe` | 7,354,300.7 cycles（3146.90 µs） | 7,144,593.6 cycles（3057.17 µs） | −2.85%，噪声区间，无关路径 |
| 2 | `ls /bin` | 364,769,657.2 cycles（156.085 ms） | 358,073,388.9 cycles（153.219 ms） | **−1.84%，基本持平；不归本轴优化** |
| 3 | `edit -c /etc/passwd` | 26,318,190.1 cycles（11.262 ms） | 22,126,991.6 cycles（9.468 ms） | **−15.93%，同向改善** |

干净 HEAD 参照：echo 3083.582 µs、ls /bin 147.594 ms、edit 10.251 ms。

- `ls /bin` 主体在目录列举后的渲染 / 字形 / 冲刷（另一轴负责），本轮未触碰；缓存只让其文件簇/目录项重读略快，−1.84% 落在噪声内，**不记为本轴收益**。
- `edit -c /etc/passwd` 打开时读一次该文件，命中路径缓存 + 簇快照，−15.93%；显示路径未改。

## 6. 正确性门禁证据

### 6.1 字节一致性（`bench fscheck`，guest 内先冷读再热读缓存命中，FNV-1a 32 位；3 次跑结果一致）

| idx | 路径 | off | max | 长度 | guest 摘要（热读） | 冷读摘要 | match | 盘上 / 源码树对照 |
|---|---|---|---|---|---|---|---|---|
| 0 | `/etc/passwd` | 0 | 24576 | 81 | `507d3a57` | `507d3a57` | 1 | 无 host 侧独立真值（见 §7.1） |
| 1 | `/bin/hello.ce` | 0 | 24576 | 312 | `bf97606e` | `bf97606e` | 1 | 参考盘 on-disk 与两树源码 FNV 均 `bf97606e` |
| 2 | `/bin/ocntools.ce` | 0 | 20480 | 20480 | `17672f84` | `17672f84` | 1 | 参考盘 on-disk `[:20480]` = `17672f84`（跨 16 KiB 的 20 KiB 读） |
| 3 | `/bin/ocntools.ce` | 16384 | 4096 | 4096 | `10d0b557` | `10d0b557` | 1 | 参考盘 on-disk `[16384:20480]` = `10d0b557`（尾段切片） |

对照方法：host 侧按参考盘 BPB（分区 LBA=1，bps=512，spc=8，rsvd=32，FAT×2，根簇=2）解析 FAT32 链提取 on-disk 字节，FNV-1a 32 位（offset basis 2166136261，prime 16777619）逐段比对；两份源码树 `extapp/bin` 摘要一致。

### 6.2 错误路径回归

`python3 tests/vprobe_regress.py`：**steps=10 ok=10 fail=0**（format / bin-has-vprobe / fr / cat / hw / str / strcpy / wr / fw / pw 全 PASS）。`fr` 用例覆盖 `file_read` 越界与空指针，仍返回负错误码；`wr`/`fw`/`pw` 覆盖写路径，簇快照与路径缓存均按 §4.1/4.2 失效。

### 6.3 显示与启动回归

- `edit -c /etc/passwd` 实机文本控制台复测：正常显示两行口令记录后回到提示符（`root:0:0:/root:/bin/shell.ce:ffffffff:`、`user:100:100:/home/user:/bin/shell.ce:2b:`）。
- `fs-after` 三次跑串口均有 `[boot] shell-ready`，无 PANIC；`meta.ok=True`。

## 7. 预期收益 vs 实际收益、限制与做不动的地方

### 7.1 假设验证

| 候选假设 | 实测依据 | 处置 | 结果 |
|---|---|---|---|
| 每次按 16 KiB 整块全量搬（mem 段贵） | before mem 仅 696 cycles/call（0.04%） | **未改**拷贝逻辑 | 假设证伪 |
| 同一文件连续读每次从头解析路径/目录（resolve 贵） | before resolve 1,238,080 cycles（66.2%） | 路径单槽缓存 | resolve −99.15%，符合预期 |
| 每次重读数据簇 IDE PIO（ide 贵） | before ide 624,243 cycles（33.4%） | 簇快照单槽缓存 | ide −96.39%，符合预期 |

预期：最大头在 resolve；实际 resolve 与 ide 合计占 before 内核段 99.6%，两段均被压下，内核段整体 −97.93%。

### 7.2 已知限制（如实标注）

1. **`/etc/passwd` 无 host 侧独立真值**：参考盘 `/etc` 目录为空，该文件由内核默认种子在首次启动时写入私有副本；环境无 mtools / python fatfs 库以外的独立提取途径。本文件仅做 guest 冷读 vs 缓存热读摘要一致（`507d3a57`，len=81）；应用程序文件则与参考盘 on-disk 字节逐段 FNV 比对一致。
2. **缓存为单槽**：路径缓存与簇快照各只存一条。多文件交替读会互相挤掉槽位，无正确性问题，只是不累积收益；当前探针为同一文件 500 次连续读，正命中该场景。
3. **resolve 段双峰**：after 三次跑 resolve 为 10,486 / 2,997 / 11,141 cycles/call，中位数 10,486；高值跑与 host loadavg / 中断噪声相关，方向稳定（before 同段 1.24M），但单次绝对值有 ±数倍波动。
4. **`ls /bin` 不归本轴**：−1.84% 为缓存侧附带波动，主体成本仍在渲染线；本轮未读渲染/字形/冲刷代码。
5. 簇快照缓存的是单簇只读副本；所有写路径（含 `fat_write_file` 链式扩展、目录项改写、格式化、切卷）均已挂失效，但未覆盖未来新增的绕开 `fat_write_cluster` 的就地写路径——后续新增写入口时必须同步失效。
6. 未测量 / 未改动：`write("x")` 渲染路径、登录口令校验、开机分段（见基线报告 §2.4、§3）。
