# CatOS 重轴性能基线（perf/heavy, `bench heavy`）

## 环境与口径

- 分支：`perf/heavy`；提交：`e17f888`；ISO：`/home/user/work/catos-perf-heavy/kernel_v25/catos.iso`
- ISO 字节：11905024，sha256 前 8 位：`d44658d5`
- QEMU：`/usr/bin/qemu-system-i386`（QEMU emulator version 6.2.0 (Debian 1:6.2+dfsg-2ubuntu6.31)）
- 命令行：`taskset -c 1 qemu-system-i386 -m 256 -smp 1 -cpu qemu32 -no-reboot -net none -display none -cdrom <iso> -boot order=d [-drive file=<private copy>,format=raw,if=ide] -serial file:<log>`
- host：nproc=2，loadavg=0.00 0.33 0.41 1/254 38955；每配置重复 3 次取中位数。
- 口径见 `bench/PERF_SPEC.md`；cycles 为 guest rdtsc 差值，us/MB/s/fps 由 guest 自校准 mhz 在 host 端换算。

## 中位数结果

| metric | n | cycles(中位) | us | bytes | MB/s | frames | fps | count | 单次 us | 分段 / 备注 | 波动 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| gfx.frame | 3 | 132861716 | 56851.398 |  |  | 6 | 105.54 |  |  | glyph=79202474 (60%); flush=53869972 (41%) | 1.8% |
| pipe.tput | 3 | 440444524 | 188465.778 | 65536 | 0.35 |  |  |  |  | sleeps=33 ticks=17 bad=0 | 0.0% |
| disk.seq | 3 | 662696918 | 283567.359 | 4194304 | 14.79 |  |  |  |  | wait=231025210 (35%); xfer=421903820 (64%) | 4.6% |
| task.spawn | 3 | 425834968 | 182214.364 |  |  |  |  | 10 | 18221.436 | setup=59262762 (14%); exit=363877852 (85%) | 4.1% |

## 瓶颈判断（分段 cycle 证据）

- **gfx.frame**：glyph/单元格绘制 `79202474`（60%），整屏 LFB 批量冲刷 `53869972`（41%）；两段同量级，字形绘制略占大头。整屏重绘 22.14 M cycles/帧，105.5 fps（中位）。
- **disk.seq**：PIO 数据搬运（每扇区 256 次 `inw`）`421903820`（64%），状态/DRQ 轮询等待 `231025210`（35%）；搬运段占大头，轮询等待约三分之一。顺序读中位 14.79 MB/s（4 MiB，raw LBA，ATA PIO）。
- **pipe.tput**：64 KiB 共用 `33` 次阻塞睡眠、`17` 个定时器 tick（@100Hz ≈ 170 ms），占总墙钟约 90%；FIFO 拷贝本身很便宜，吞吐被调度/唤醒粒度卡住（中位 0.35 MB/s，bad=0）。
- **task.spawn**：地址空间/任务表建立 `59262762`（14%），用户态入口 + 退出回收 `363877852`（85%）；退出/回收段是 spawn→exec→exit 全链路的大头，单次退出回收约 15.57 ms（全链路 18.22 ms/次，N=10）。

**最该优先优化的一处**：task.spawn 的退出/回收段（exit 占约 85%，单次约 15.6 ms）；其次 pipe 受 100 Hz 唤醒粒度限制（阻塞约 90% 墙钟）。证据见上表对应分段 cycle 占比；其余两段（gfx glyph/flush、disk wait/xfer）为同量级或次优矛盾，留待优化轮再拆。

## 测量方法与分段口径

- **gfx.frame**：总 cycles 为 F=6 次 `con_repaint_frame()`（整屏 clear + 全网格重绘，最坏路径）；flush 段为 F 次 `gfx24_clear()` 整屏 LFB 填充；glyph 段 = 总 − flush（网格遍历 + 字形光栅化进 LFB）。未计入逐字符 `con_write` 的换行/回退路径。
- **disk.seq**：总量走真实 `ata_read()` 驱动（drive 0，raw LBA，4 MiB / 8192 扇区，32 扇区/次）；wait/xfer 分段由同一端口序列的探针在同区域复读测得（wait=BSY/DRQ 轮询，xfer=每扇区 256 次 `inw`），与 `ata.c: ata_read` 的端口顺序一致。
- **pipe.tput**：ring0 助手 `kpipe_write/kpipe_read` 复用内核静态 FIFO（PIPE_BUF=4096）与 `task_sleep_ticks` 阻塞语义；16 字节周期模式 `0123456789ABCDEF`，64 KiB，256 B 分块；sleeps=阻塞次数，ticks=经过的定时器 tick（PIT 100 Hz，1 tick=10 ms）。
- **task.spawn**：N=10 次 `/bin/hello.ce`；setup 段 = `task_spawn_ce()`（槽位/FS 解析/地址空间/镜像装载/栈与 vsyscall 建立）；exit 段 = `task_wait_child()` 到子任务退出回收（ring3 入口、ce_main、exit syscall、清理回收）。
- 无盘批次（gfx、pipe）与带盘批次（disk、task）分 tag 跑；带盘用例 的私有磁盘副本由 qrun 每次复制，不挂仓库原盘。

## 原始日志

- `baseline-heavy-nodisk` — `/home/user/work/catos-bench-runs/baseline-heavy-nodisk`
- `baseline-heavy-disk` — `/home/user/work/catos-bench-runs/baseline-heavy-disk`
