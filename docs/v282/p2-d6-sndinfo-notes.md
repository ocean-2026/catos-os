# v2.82 · P2-D6 `sndinfo` PCM 诊断字段 — 作业记录

分支来源：`v282/p1-2-3-shell-text`（D6 改动在收口前未提交，由父代理以补丁应用到主线
`459f582` 之上，逐 hunk 干净应用）。

## 1. 做了什么

- `kernel_v25/include/es1371.h`：新增三个诊断枚举
  `ES1371_INFO_PERIOD/AVAIL/UNDERRUN`，注释写清「-1 表示不可导出，调用方显示 n/a，
  不编造 0」。
- `kernel_v25/src/es1371.c`：`es1371_info()` 新增三个 case——
  - `PERIOD`：读 `DAC1_SCOUNT` 低半字（驱动 `arm_dma()` 写入的是 `frames-1`），
    `+1` 还原为一帧 DMA 块的帧数；未 arm 时返回 0 而不是伪造满缓冲。
  - `AVAIL`：读 `DAC1_FRAMECNT`（低半字=size，高半字=已消费计数），
    返回 `size - cnt + 1`，并做越界夹取。
  - `UNDERRUN`：本驱动暂无 xrun 计数，恒定返回 -1（= n/a）。
- `kernel_v25/src/shell.c`：`si_pcm_block()` 在 2ch S16LE（PCI 卡）档位输出真实
  `period / avail / underrun`；非 PCI 档位保持原「not exported by driver」文案。
- `kernel_v25/src/posix.c`：把 `sndinfo` 的 usage/flags 补齐到 v2.65 已实现的长短选项
  （-h -v -l -p -c -d N -w -j -s -q -t），`sndtest` 加 `[FREQ MS]` 参数位。
- `tests/p2_d6_sndinfo_probe.py`（新增）：三档场景 `es1370 / sb16 / none`，
  各用独立 monitor 端口 4471/4472/4473。

## 2. 验证命令与真实输出（云主机 QEMU，`-device es1370,audiodev=snd0`）

```
python3 tests/p2_d6_sndinfo_probe.py es1370     -> RC=0
  sndinfo -p : Buffer size: 16384 frames; period: 0 frames  avail: 0 frames  underrun: n/a
  sndtest    : snd: playing 440 Hz for 800 ms / SNDTEST PASS
  再过一次 -p: Buffer size: 16384 frames; period: 16384 frames avail: 15584 frames underrun: n/a

python3 tests/p2_d6_sndinfo_probe.py sb16       -> RC=0
  sndinfo -p : Buffer size: 32768 frames (period/avail not exported by driver)
               Underrun count: n/a

python3 tests/p2_d6_sndinfo_probe.py none       -> RC=0
  sndinfo -p : sndinfo: audio subsystem not initialized / no sound card
```

三张截图：`artifacts/v282/p2-d6/01-sndinfo-es1370-device.png`、
`02-sndinfo-sb16-pci-absent.png`、`03-sndinfo-no-device.png`。

## 3. 边界与未做

- `underrun` 仍是 n/a：驱动没有 xrun 记账，**没有**为了好看编一个 0。
- 没有 ES1371（0x1271/0x1371 真机）可测，只有 QEMU ES1370（0x1274/0x1370，同一驱动
  接受两种 device id）；ES1371 专属寄存器差异未单独验证。
- capture 路径仍 n/a（驱动本来就没有 capture）。

## 4. 复跑方式

```sh
cd kernel_v25 && sh build.sh iso
export PATH=/home/user/bin:$PATH
python3 tests/p2_d6_sndinfo_probe.py es1370   # 需要 QEMU 支持 -device es1370
```
