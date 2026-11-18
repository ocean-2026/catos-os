# 开机耗时打点（v2.57 / 2026-11-19 复测）

数据来自内核 `btime_mark()` 串口面包屑（`[btime]`），采集方式：

```sh
/home/user/bin/qemu-system-i386 -m 256 -no-reboot -net none \
  -cdrom kernel_v25/catos.iso -boot order=d -display none -monitor none \
  -serial file:/tmp/btime.log
grep -o "\[btime\].*" /tmp/btime.log
```

## 实测阶段耗时（本机 QEMU 6.2，-m 256，无磁盘）

| 标记 | 阶段 | 累计 ms | 相对上一标记 |
|---|---|---|---|
| K0 | `kernel_main` 进入 | 0 | — |
| K1 | vga + 键盘初始化 | 0 | 0 |
| K2 | 分页开启 | 0 | 0 |
| K3 | intr/tasks + `sti` | 10 | 10 |
| K4 | 内存 + CPU 校准 | 110 | 100 ← **最大单步** |
| K5 | `snd_init` | 110 | 0 |
| K6 | `usb_start` | 110 | 0 |
| K7 | `net_init`（无网卡立即返回） | 110 | 0 |
| K7b | `mouse_init`（8042 aux 初始化） | 110 | 0 |
| S0 | `shell_run` 进入 | 110 | 0 |
| F0 | `fs_init` 进入 | 110 | 0 |
| A0→A13 | ATA 4 槽探测（无盘，逐槽 0ms） | 110 | 0 |
| F2 | FAT 卷探测完成 | 110 | 0 |
| S1 | `fs_init` 结束 | 110 | 0 |
| S2 | `lang_init` | 110 | 0 |
| S3 | `env_init`（读 `/etc/env`） | 110 | 0 |
| S4 | `ce_init`（应用清单） | 120 | 10 |
| S5 | user/acl/session | 120 | 0 |
| S9 | **shell 提示符就绪** | 120 | 0 |

## 结论与热点判断

- 冷启动到提示符 **≤120 ms**（-m 256，无磁盘、无网卡）。这个数量级没有做优化的必要。
- 唯一超过 10 ms 的阶段是 **K4「内存 + CPU 校准」100 ms**：`cpu_calibrate()` 用 PIT 做
  busy-wait 测 cycles/秒，属**有意等待**，不是浪费——它决定了 TSC 超时（蓝屏 30s、
  watchdog 倒计时）的换算精度。把校准窗口从 100 ms 缩到 20 ms 会让 MHz 估算误差放大
  到 ±5%，蓝屏超时误差 ±1.5 s，得不偿失；本批**不改**。
- 有盘场景的分层（ATA 探测 → FAT → part_scan）在无盘测试里全为 0 ms；真机带盘时
  A0→F3 段的耗时取决于盘速与簇数，属于 I/O 侧，本轮不做专项优化。
- `mouse_init`（新增，8042 aux 协商）**未增加可测耗时**（K7b 与 K7 同毫秒）。
- `boot_apply_videomode()`（v2.56 新增）在 S3 后、S4 前执行；进入 VBE24 后端本身由
  BIOS 提供模式，实测未引入可测延迟（S3 110ms → S4 120ms 的 10 ms 主要是 `ce_init`）。

后续若要再压，可优先看「有盘 + 网络」组合场景的 ATA/net 初始化重试路径（`ata_scan`
在 4 个空槽上各打一次 400ns 的 status 轮询，属于固定成本）。当前无优化项落地，结论：
**保持现状，热点为有意等待**。
