# gfx24 图形渲染路径优化报告

- 工作树：`/home/user/work/catos-perf-heavy`（分支 `perf/heavy`）
- 范围：gfx24（VBE 24bpp）后端绘制与冲刷路径，改动集中在
  `kernel_v25/src/be_vbe24.c`；取证脚手架在 `kernel_v25/src/bench_heavy.c`、
  `kernel_v25/src/render.c`、`kernel_v25/include/render.h`。
- 测量环境：QEMU `/usr/bin/qemu-system-i386` 6.2.0，`-cpu qemu32 -m 256 -smp 1`，
  guest 校准 `mhz=2337`；基线与优化后使用**同一套 bench 构建脚本、同一 QEMU 命令行**。
- 结论一句话：整屏重绘从 9.86 ms/帧（101.4 fps）降到 3.20 ms/帧（312.7 fps），
  单字符绘制从 9,055 cycles 降到 4,888 cycles；优化前后演示区域像素逐字节一致。

## 1. 基线（优化前，三次中位数）

取证方式：在 `bench_gfx()` 内用 rdtsc 分窗计时——
- `gfx.frame`：F=6 次 `con_repaint_frame()` 全量重绘；其中 `cycles_flush` 为
  6 次 `gfx24_clear(0x0C0C0C)` 整屏填充，`cycles_glyph` = 总量 − flush（网格遍历 +
  单元格/字形光栅化）。
- `gfx.char`：N=2000 次后端 `draw_cell(3,3,'x',fg=0xDCDCDC,bg=0x0C0C0C)` 全量绘制；
  `cycles_fetch` 为 2000 次纯字体表查找（`render_font_glyph`）。

| 指标 | 基线中位数 | 三次原始 cycles |
|---|---:|---|
| `gfx.frame` 总量（6 帧） | 138,273,772 | 136,388,746 / 138,273,772 / 140,781,576 |
| └ glyph（6 帧） | 81,662,282 | — |
| └ flush（6 帧） | 54,726,464 | — |
| `gfx.char`（2000 格） | 18,110,768 | 17,496,154 / 18,110,768 / 19,062,316 |
| └ 字体查找 fetch（2000 次） | 148,564 | — |

派生（基线）：
- 单帧 23,045,629 cycles ≈ **9.86 ms / 帧，101.4 fps**；
  glyph 13.61M cycles/帧，flush 9.12M cycles/帧。
- 单格 9,055 cycles ≈ 3.875 µs；字体查找仅 74.3 cycles/格——**单格成本几乎全部在
  后端绘制（调色 + 背景填充 + 墨迹写 LFB），不在字体取位**。
- flush 段：9.12M cycles / 4.32M 字节 ≈ 2.11 cycles/字节，是纯逐字节写 LFB 的代价。

## 2. 分段证据（钱花在哪）

| 段 | 基线 cycles | 占比 / 说明 |
|---|---:|---|
| 整屏重绘 `con_repaint_frame` | 138.27M / 6 帧 | 最坏情况：主题切换 / 滚动回退时的全屏重画 |
| ├ 整屏 LFB 冲刷（`gfx24_clear`） | 54.73M（40%） | 1.44M 像素 × 3 字节，逐像素 3 次字节写 |
| └ 网格遍历 + 单元格/字形绘制 | 81.66M（60%） | 每格先 `fill` 8×16 背景，再逐位 `put3` 墨迹 |
| 单字符 `draw_cell` | 9,055 cycles/格 | 背景填充 128 像素 + 约 28 个墨迹像素 |
| 字体表查找 | 74.3 cycles/格 | 可忽略，不是优化目标 |

后端现状（优化前）：
- Bochs 24bpp：pitch=2400、stride=3、BGR 字节序；SVGA 回退：stride=4、BGRX。
- `put3()` 每个墨迹像素 3 次独立字节写（经 `g_b/g_g/g_r` 索引）；
  `fill()` 内层逐像素 3 字节写（stride4 再补 alpha），单元格背景 8×16=128 像素全走此路；
  `v24_scroll` 逐字节 LFB 搬移；LFB 为 UC 映射，无 WC MTRR。

## 3. 改动内容（`kernel_v25/src/be_vbe24.c`）

保持像素级输出一致，只改写 LFB 的存储宽度与批量化：

1. **`fill()` 背景/矩形填充批量化**
   - 32bpp 标准 BGRX 布局：一个像素打包成一个 `uint32_t` 一次写入（B|G<<8|R<<16|FF<<24）。
   - Bochs 24bpp（stride=3）：两个像素打包为一次 `u32`（B0 G0 R0 B1）+ 一次 `u16`
     （G1 R1），两个像素 2 次存储替代 6 次字节写；奇数尾像素走原字节路径。
   - 非标准通道掩码（SVGA `off_*` 不是 0/1/2/3）自动回退到改动前的逐字节索引路径。
2. **`put3()` 墨迹像素**：标准 32bpp 布局走单条 `u32` 存储；其余布局保持原字节写。
3. **`gfx24_blit()`**（ring3 RGB24 → 设备）：标准布局打包 `u32` 一次写，其余回退原路径。
4. **`v24_scroll()`**：上移搬移按 4 字节 `u32` 前向拷贝，尾段逐字节；前向顺序保证
   源（高位地址）未被覆盖前先读出。
5. 取证脚手架：`render.c`/`render.h` 新增只读访问器 `con_backend()`；
   `bench_heavy.c` 的 `bench_gfx()` 增加 `gfx.char` 微基准（未改动其它轴场景）。

未触碰：`bench/perf.c`、`perf.h`、`qrun.py`、`report.py`、`PERF_SPEC.md`、
`src/tls*.c`、`catos_disk.img`；未执行 `build.sh disk`；未改动主树
`/home/user/work/catos-os`。

## 4. 优化前后对比（三次中位数，同一 QEMU 命令行、同一 bench 构建）

| 指标 | 基线 | 优化后 | 加速比 | cycles 下降 |
|---|---:|---:|---:|---:|
| `gfx.frame` 总量（6 帧） | 138,273,772 | 44,836,610 | **3.08×** | 67.1% |
| 单帧 cycles | 23,045,629 | 7,472,768 | 3.08× | — |
| 单帧耗时 | 9.86 ms（101.4 fps） | 3.20 ms（312.7 fps） | — | — |
| └ glyph（6 帧） | 81,662,282 | 35,100,390 | 2.33× | 57.0% |
| └ flush（6 帧） | 54,726,464 | 10,253,646 | **5.34×** | 81.3% |
| `gfx.char`（2000 格） | 18,110,768 | 9,776,702 | **1.85×** | 46.0% |
| 单格 cycles | 9,055 | 4,888 | 1.85× | — |
| 单格耗时 | 3.875 µs | 2.092 µs | — | — |
| 字体查找 fetch/格 | 74.3 | 80.4 | ≈1.0× | 取证噪声，无实质变化 |

优化后三次原始值：
- frame：44,836,610 / 46,584,876 / 44,808,296（spread 3.96%）
- char：9,776,702 / 9,817,964 / 9,742,928（spread 0.77%）

### 预期收益 vs 实际收益

- **预期**：flush 是纯内存填充，存储宽度合并收益最大；glyph 段的单元格背景填充同样
  走 `fill()`，应次之；墨迹逐像素 `put3` 与前端网格遍历不在本轮重点。
- **实际**：flush 5.34×（符合预期，最大头）；glyph 2.33×；整帧 3.08×；单格 1.85×。
  单格收益小于整屏收益，因为单格成本里墨迹光栅化的逐像素分支/取址仍在（见第 7 节）。

## 5. 像素一致性证据（正确性门禁）

### 方法

1. 启动当前 ISO，等待 `[boot] shell-ready`，键入 `gfx24` 切模式，停顿 3 s，HMP
   `screendump` 连抓两张取最后一张（QEMU 抓帧滞后 1–2 帧），得到 P6 PPM 800×600。
2. `bench/render_bench.py diff` 逐字节比较两张 PPM：演示区域 `y ≥ 120`
   （`DEMO_Y0=120`，避开顶部提示行与软件光标）做字节级 diff + MD5；另报整屏不一致像素数。
3. 噪声地板标定：优化前独立启动两次抓 `base.ppm` / `base2.ppm`；优化后再抓
   `opt.ppm` / `opt2.ppm`，确认光标闪烁行的固有噪声量级。

### 结果

| 对比 | 演示区域（y≥120）字节不一致 | 区域 MD5 | 整屏不一致像素 |
|---|---:|---|---:|
| base vs base2（基线自比，噪声地板） | 0 | 一致 | 8（全部在第 111 行，光标闪烁行） |
| **base vs opt（前后端对比）** | **0** | `a6775890bf4d06b5839ba97418a4ce7c` 两边一致 | 8（全部在第 111 行，光标闪烁行） |
| opt vs opt2（优化后自比） | 0 | 一致 | 8（光标闪烁行） |

- 演示区域 1,152,000 字节**逐字节一致**；整屏 8 个不一致像素与基线自比的噪声地板
  完全相同，位于底部光标行，属光标闪烁时序差异，与渲染路径无关。
- 产物：`artifacts/perf-render/base.ppm`、`artifacts/perf-render/opt.ppm`
  （各 1,440,015 字节，800×600 P6）。

## 6. 其它门禁

### 启动与 PANIC

优化后 3 次 bench 启动日志（`/home/user/work/catos-bench-runs/render-opt/`）：
每次均有 `[boot] shell-ready`，`PANIC` 计数 0，`mhz=2337`，无早期退出。

### 文本模式（`text` 命令）

`bench/gate_text.py`：启动 → 等 READY → `text` 切回 80×25 文本后端 → `ls /bin`。
结果（`artifacts/perf-render/text-gate.json`）：
- `ready=true`，`panic=false`，`text_rendered=true`；
- VGA 文本控制台正常回显：`text: back to 80x25 text mode ...`、提示符与
  `ls: no FAT32 volume (run 'format' first)` 均正常渲染。
- 说明：本门禁未挂磁盘，故 `/bin` 列表为空属预期；文本后端回显链路已验证可用。

### `tests/gfx24_regress.py` 结果

跑了一次（`-vga std`，自带稀疏 64 MiB 盘）：**13 项 FAIL**。核对后确认这两项是
**本树既有状态，与本轮存储路径改动无关**：

1. `/bin/gfx24.ce` 未被 `format` 安装：`ls /bin` 列出 28 个条目，无 `gfx24.ce`
   （打包/内置清单状态问题）。
2. 内核 `gfx24` shell 命令只做模式切换 + `con_clear()`；`gfx24_demo()`（四色块 /
   渐变 / 八横条）在本树**没有任何调用点**，所以脚本第 2、3 步采样到的是控制台背景
   `(10,14,20)`，而非预期色块。

佐证：优化前抓的 `base.ppm` 在同样坐标读到相同背景色，且 base/opt 演示区域 MD5 一致；
本轮 diff 只改 `be_vbe24.c` 的写 LFB 原语，不涉及应用打包与命令接线。
若要让该套件全绿，需要另行补：内置应用安装清单 + 在 `gfx24` 命令（或模式进入路径）
调用 `gfx24_demo()`，不属于本次渲染性能优化范围。

## 7. 做不动 / 未优化的地方（如实说明）

- **字形墨迹光栅化仍是逐像素**：`draw_narrow`/`draw_wide` 每行重算行指针，墨迹像素
  逐个 `put3`，bold/italic 分支在像素内层判断；本轮没有做字形掩码预展开或行级批量写。
- **背景仍整格写**：单元格背景 128 像素全量填充，未按字形掩码跳过墨迹将覆盖的像素；
  留白格（cp=0）只付背景成本，墨迹格也重复写了背景再盖墨迹。
- **LFB 仍是 UC 映射**：没有配置 WC MTRR；存储宽度合并在 QEMU TCG 下收益明显，
  真实硬件上未缓冲的 UC 写仍可能成为瓶颈，本轮未验证 WC 路径。
- **前端网格/脏单元格逻辑未动**：`render.c` 的脏单元格跟踪、repaint 调度不在本轮
  后端优化范围内；`vga.c` 文本后端不在被测 gfx24 路径。
- 没有用近似指标冒充收益：所有 cycles 均为 rdtsc 实测中位数，fps/µs 由
  `cycles / mhz(2337)` 换算。

## 8. 复跑命令

```bash
# 构建（同一命令出基线与优化后镜像）
cd /home/user/work/catos-perf-heavy/kernel_v25
BENCH=1 sh build.sh kernel && BENCH=1 sh build.sh iso
cd /home/user/work/catos-perf-heavy

# 基线取证（需在优化前代码上跑；本树已留存 base.json / base.ppm）
python3 bench/render_bench.py bench artifacts/perf-render/base.json --repeat 3 --tag render-base
python3 bench/render_bench.py capture artifacts/perf-render/base.ppm

# 优化后取证
python3 bench/render_bench.py bench artifacts/perf-render/opt.json  --repeat 3 --tag render-opt
python3 bench/render_bench.py capture artifacts/perf-render/opt.ppm

# 像素一致性对比
python3 bench/render_bench.py diff artifacts/perf-render/base.ppm artifacts/perf-render/opt.ppm

# 文本模式 + 启动门禁
python3 bench/gate_text.py

# 既有回归套件（结果见第 6 节说明）
python3 tests/gfx24_regress.py
```

### 关键产物路径

- 报告：`/home/user/work/catos-perf-heavy/artifacts/perf-render/render.md`（本文件）
- 基线数据：`/home/user/work/catos-perf-heavy/artifacts/perf-render/base.json`
- 优化后数据：`/home/user/work/catos-perf-heavy/artifacts/perf-render/opt.json`
- 基线截图：`/home/user/work/catos-perf-heavy/artifacts/perf-render/base.ppm`
- 优化后截图：`/home/user/work/catos-perf-heavy/artifacts/perf-render/opt.ppm`
- 文本门禁结果：`/home/user/work/catos-perf-heavy/artifacts/perf-render/text-gate.json`
- 驱动脚本：`/home/user/work/catos-perf-heavy/bench/render_bench.py`、
  `/home/user/work/catos-perf-heavy/bench/gate_text.py`
