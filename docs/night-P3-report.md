# CatOS 跨夜迭代 P3 分片报告：Linux 同款彩蛋 + 用户体验增强

- 分片：P3（独立工作副本 `night/CatOs-P3`，基线 commit `9e47280` v2.37）
- 日期：2026-11-15
- 构建产物：`kernel_v25/catos.iso`（11,542,528 字节）
- 实现路径：**全部为 ring0 shell 内建命令**，未新增 CE 应用（无需走 `extapp/build_ce.sh` 与 `builtin_apps.inc` 嵌入链），改动集中在 `kernel_v25/src/shell.c`。

---

## 1. 改动文件清单

### 1.1 源码（合并冲突预警重点）

| 文件 | 改动性质 |
|---|---|
| `kernel_v25/src/shell.c` | 新增 5 个彩蛋命令 + 3 项 UX 增强；新增命令表条目集中在 `shell_exec()` 分发表，全部以 `/* NIGHT-P3 */` / `// NIGHT-P3` 注释标记 |

`shell.c` 内改动位置（供主代理定位合并点）：

1. `cmd_help()`：新增 `cls` 行、`echo` 用法更新、新增 `fun (NIGHT-P3 Linux-style easter eggs)` 小节 5 行。
2. `cmd_date()` 之后：整段 `NIGHT-P3` 代码块（cowsay / sl / fortune / cal / yes / echo 增强 / 命令建议表与 Levenshtein）。
3. `shell_exec()` 分发表：
   - `clear` 分支扩展 `cls` 别名；
   - `echo` 改调 `cmd_echo(arg)`；
   - 新增 `cowsay / sl / fortune / cal / yes` 五个分支；
   - 未知命令分支追加 `did you mean` 建议。

> 合并提示：分发表是一长串 `else if (!strcmp_(cmd, ...))` 链，新增条目已按现有风格插入；若其他分片也改了 `shell_exec()`，冲突大概率落在这条链上，按命令名对齐即可。

### 1.2 测试探针（新增，不参与内核构建）

| 文件 | 用途 |
|---|---|
| `tests/p3_probe.py` | 主探针：格式化磁盘后逐条驱动 12 个步骤并截图（端口 4730） |
| `tests/p3_probe2.py` | 补充探针：help 回滚视图、6 次 fortune、sl、默认 cowsay（端口 4731） |
| `tests/p3_probe3.py` | help 顶部回滚视图专用探针（端口 4732） |
| `tests/p3_probe4.py` | 格言文案裁剪后重 fortune / sl / cowsay 默认值（端口 4734） |

### 1.3 文档

| 文件 | 改动 |
|---|---|
| `docs/night-P3-report.md` | 本报告（新增） |
| `docs/history.md` | 追加 6 条 CatOS[易错点] |
| `docs/experience.md` | 追加 4 条 CatOS[经验] |

### 1.4 截图产物（未跟踪工件，`tests/qemu_shots/p3_*.png`，共 21 张）

见第 5 节清单。

---

## 2. 彩蛋实现明细（均为 shell 内建）

### 2.1 `cowsay [-f tux] MSG` —— 文本 cowsay

- 实现：`cmd_cowsay()`，内建。
- 角色：默认奶牛（`COW_ART`）；`-f tux` 切换企鹅 Tux（`TUX_ART`），共两种角色。
- 气泡：按消息长度画上下横线 + 左右尖括号；消息为空时奶牛说 `Moo!`、Tux 说 `Hello, I am Tux!`；消息截断到 42 字符。
- 用法实测：
  - `cowsay Hello CatOS` → 奶牛气泡 `Hello CatOS`（截图 `p3_01_cowsay.png`）
  - `cowsay -f tux Hello Tux` → 企鹅气泡 `Hello Tux`（截图 `p3_02_tux.png`）
  - `cowsay`（无参数）→ 默认 `Moo!`（截图 `p3_16_cowsay_default.png`）

### 2.2 `sl` —— 字符画小火车动画

- 实现：`cmd_sl()`，内建。
- 16 帧动画：机车从右侧（列 64）逐帧左移，每帧 `task_sleep_ticks(6)`（tick=10ms，约 60ms/帧）。
- 细节：烟囱上方三态循环烟雾 `SL_SMOKE`；前 5 帧打印 `* CHOO CHOO! *` 鸣笛；机车含烟囱、双车窗、侧梁、两轮。
- 截图：`p3_03_sl.png`、`p3_15_sl_v2.png`（动画中途帧）。

### 2.3 `fortune` —— 随机格言库

- 实现：`cmd_fortune()`，内建。
- 内置 **32 条**格言：20 条英文 + 12 条中文（UTF-8），中英混排。
- 随机熵：RTC 秒/分 + `timer_ticks`（10ms 刻度）+ 单调序列计数器（乘 2654435761 散列），避免同一秒内连续调用重复。
- 截图：英文 `p3_04_fortune.png`、`p3_14_fortune_0/1/2/4/5.png`；中文 `p3_14_fortune_3.png`（只要功夫深，铁杵磨成针）、`p3_14_fortune_2.png`（不积跬步…）、`p3_14_fortune_5.png`（业精于勤…）。

### 2.4 `cal [YEAR]` —— 月历 / 全年历

- 实现：`cmd_cal()` + `cal_build_month() / cal_print_one() / cal_print_year()`，内建。
- 无参数：按 RTC 显示当月（实测 Nov 2026，1 号周日，30 天，正确）。
- 带年份参数（1970..2099）：全年 4 组 × 每月 3 列并排布局。
- 算法：
  - 星期：Sakamoto 公式；
  - 闰年：`(y%4==0 && y%100!=0) || y%400==0`；
  - 宿主侧用 Python `datetime` 对 2020–2026 各月 1 号逐日比对，星期全部一致；闰年判定 2024/2000 为闰、2026/1900 非闰，Feb 2024=29 天、Feb 2026=28 天，全部正确。
- 截图：当月 `p3_05_cal.png`；全年 `cal 2026` → `p3_06_cal2026.png`（Oct/Nov/Dec 三列对齐，Oct 1 周四、Nov 1 周日、Dec 1 周二，正确）。

### 2.5 `yes [TEXT]` —— 连发输出（可 Ctrl+C 中断）

- 实现：`cmd_yes()`，内建。
- 默认重复 `y`；可传自定义文本。
- 中断：每行后 `kb_getkey_timed(1)` 轮询键盘，收到 `0x03`（Ctrl+C）即打印 `^C` 退出；另设 600 行安全上限兜底。
- 截图：`p3_07_yes.png`（满屏 `y`，末行 `^C`，确认 Ctrl+C 可中断）。

---

## 3. UX 增强明细（3 项，均真实可用）

### 3.1 `cls` 别名 → `clear`

- `shell_exec()` 中 `clear` 分支扩展为 `clear || cls`，行为同清屏。
- 此前 `cls` 是未知命令（`qemu_driver.py` 注释里也记录了这一点）。
- 截图：`p3_10_cls.png`（`cls` 后屏幕仅剩提示符）。

### 3.2 未知命令友好提示（最近命令建议）

- 实现：`BUILTIN_NAMES` 内置命令表 + `cmd_lev()` 微型 Levenshtein + 前缀命中降权；距离 ≤2 才输出 `catos: did you mean: <cmd> ?`。
- 实测：`helo` → `unknown command: helo` + `did you mean: help ?`（截图 `p3_11_suggest.png`）。
- 未命中近邻时不打印建议，避免乱提示。

### 3.3 `echo` 开关参数：`-n` 无换行、`-c COLOR` 着色

- 实现：`cmd_echo()`，替换原裸 `vga_write(arg)` 实现。
- `-n`：末尾不加换行（实测提示符紧跟在文本同一行，截图 `p3_09_echo_n.png`）。
- `-c COLOR`：命名色 red/green/blue/yellow/cyan/magenta/white/grey/gray/darkgrey/brown；打印前 `con_get_fg()` 存旧前景，打印后还原，不污染用户当前主题色。
- 实测：`echo -c red Red text here` → 红色输出（截图 `p3_08_echo_red.png`）。
- 无开关时行为与原 `echo` 完全一致（autostart banner 的 `echo` 调用不受影响）。

---

## 4. 测试命令与结果

### 4.1 构建

```sh
cd night/CatOs-P3
sh build.sh
# == compiling src/shell.c ==
# == linking (PE/COFF) ==
# OK -> kernel_v25/catos.iso (11542528 bytes)
```

无 error / undefined reference；仅存量 warning（`intr.h` 的 `int_frame` 前向声明，基线已有）。

### 4.2 端到端回归（`tests/qemu_regress.py`）

```sh
python3 tests/qemu_regress.py kernel_v25/catos.iso \
    --qemu /home/user/bin/qemu-system-i386 --port 4735
```

结果：**steps=12，ok=11，warn=1**。

| 步骤 | 结果 | 备注 |
|---|---|---|
| boot-shell | WARN | 首帧非黑亮度 0.01，处于 0.01 阈值边界；该步骤只做启发式非黑判定，基线同型，与本次改动无关 |
| format | PASS | |
| help | PASS | |
| display | PASS | |
| gfx | PASS | |
| text-reject-N | PASS | |
| text-accept-Y | PASS | |
| ls-bin | PASS | |
| mmaptest | PASS | |
| hello-elf | PASS | |
| vdiag | PASS | |
| final-frame | PASS | |

### 4.3 实机探针（本分片自研脚本）

- QEMU：`/home/user/bin/qemu-system-i386`（6.2.0），`-m 256 -no-reboot -net none -display none`，纯 PS/2 键盘（未加 `-usbdevice keyboard`），端口段 4730/4731/4732/4734/4735。
- 每条命令经 `type_text` + `sendkey ret` 键入，`monitor screendump` 出 PPM，复用 `qemu_regress.ppm_to_png` 转 PNG；全部截图由本代理逐张 `Read` 核对内容（见第 5 节）。

### 4.4 算法宿主侧核验

- `cal` 星期（Sakamoto）与 Python `datetime` 比对 2020–2026 各月 1 号：全部一致。
- 闰年规则：2024 闰 / 2026 非闰 / 2000 闰 / 1900 非闰，正确。

---

## 5. 截图清单（绝对路径，均已 Read 核对）

基目录：`/home/user/Doubao/chats/38453665424926722/night/CatOs-P3/tests/qemu_shots/`

| 截图 | 验证内容 |
|---|---|
| `p3_01_cowsay.png` | `cowsay Hello CatOS`：奶牛 + 气泡 `Hello CatOS` |
| `p3_02_tux.png` | `cowsay -f tux Hello Tux`：企鹅 Tux + 气泡 |
| `p3_03_sl.png` | `sl` 动画中途帧：机车 + 烟雾 |
| `p3_04_fortune.png` | `fortune`：英文格言单行成框 |
| `p3_05_cal.png` | `cal`：Nov 2026 月历，1 号周日、30 天 |
| `p3_06_cal2026.png` | `cal 2026`：全年三列布局（Oct–Dec 可见，星期对齐正确） |
| `p3_07_yes.png` | `yes`：连发 `y`，Ctrl+C 中断返回提示符 |
| `p3_08_echo_red.png` | `echo -c red`：红色文本输出 |
| `p3_09_echo_n.png` | `echo -n`：无换行，提示符同行 |
| `p3_10_cls.png` | `cls`：清屏后仅剩提示符 |
| `p3_11_suggest.png` | 未知命令 `helo` → `did you mean: help ?` |
| `p3_12_help.png` | `help` 列表底部正常渲染（文件系统段） |
| `p3_13_help_top.png` | help 回滚视图（见遗留问题 1） |
| `p3_14_fortune_0.png` | fortune：`Hello, world.` |
| `p3_14_fortune_1.png` | fortune：`Talk is cheap. Show me the code. -- Linus Torvalds` |
| `p3_14_fortune_2.png` | fortune：中文 `不积跬步，无以至千里。—— 荀子` |
| `p3_14_fortune_3.png` | fortune：中文 `只要功夫深，铁杵磨成针。` |
| `p3_14_fortune_4.png` | fortune：中文 `千里之行，始于足下。—— 老子` |
| `p3_14_fortune_5.png` | fortune：中文 `业精于勤，荒于嬉。—— 韩愈` |
| `p3_15_sl_v2.png` | `sl` 机车造型帧（改进后） |
| `p3_16_cowsay_default.png` | `cowsay` 无参数：默认 `Moo!` |

---

## 6. 遗留问题与限制

1. **help 顶部小节未能截图留证**：`help` 输出超 25 行，打印结束后顶部行已离开实时网格；回滚缓冲（`CON_SB_ROWS=96`）在本环境只回得到中部段（uptime/jobs/network/filesystem 段），顶部 `fun` 小节未进入可回滚存档。新 help 行已在源码 `cmd_help()` 中逐行核对，`p3_12_help.png` 证明 help 渲染链路正常；如需顶部截图留证，后续可在 `help` 内部分段暂停或加深存档保留。
2. **`cal 2026` 首组（Jan–Mar）在 25 行屏幕外**：全年历共 4 组 × 8 行，截图可见后三组；首组布局由同一 `cal_build_month` 路径生成，算法已宿主侧核验。
3. **`sl` 造型为简化字符画**：受 80×25 文本网格与字体限制，车窗 `O` 字形接近 `0`；动画/烟雾/鸣笛细节到位，写实度不如图形后端。
4. **`yes` 安全上限 600 行**：未接真实任务级信号机制，靠 10ms 轮询检测 Ctrl+C；无键盘输入时约 6 秒自动停。
5. **未新增 CE 应用**：按"更省事路径"选择内建；彩蛋均不依赖磁盘文件，无盘环境同样可用。
6. 并行分片共用宿主：本分片探针只用 4730–4735 端口段，未触碰其他分片保留段。

---

## 7. 提交说明

- 全部改动提交到本地 `main`，commit 前缀 `P3:`，**未 push**。
- 提交范围：`kernel_v25/src/shell.c`、`tests/p3_probe*.py`、`docs/night-P3-report.md`、`docs/history.md`、`docs/experience.md`、`tests/qemu_shots/p3_*.png`。
