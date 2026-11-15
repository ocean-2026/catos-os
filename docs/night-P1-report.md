# CatOS P1 分片报告：PE/EXE 初步支持（三条路径）

> 工作副本：`/home/user/Doubao/chats/38453665424926722/night/CatOs-P1`（基线 commit `9e47280 v2.37`）
> 端口段：4710（P2=4720，P3=4730）。所有改动只在本副本，**未 push**。
> 验收对象：32 位 x86 内核，QEMU/i386，GRUB multiboot，FAT32，CE01 自有格式，无动态链接。

## 1. 结论

三条路径全部落地并通过实机 QEMU 端到端验收（`tests/p1_pe_check.py`，5 项检查 0 失败）：

| 路径 | 说明 | 实机结果 |
|---|---|---|
| A. convert-on-run | shell `run` 检测 MZ 魔数，交互询问是否转 CE01 并保存（默认同名 `.ce`，可改名）；非交互/管道/后台/脚本场景跳过询问并继续直接加载 | 询问弹出、保存为 `/bin/PESAMPLE.ce`、转换产物可运行，截图见 01/03b/03c/03d |
| B. `spawn_load_pe()` | 内核新增直接 PE32 加载器：DOS header → PE sig → COFF → Optional header → section headers → entry，段映射/拷贝后跳转；导入/导出表忽略；不支持的 PE 返回明确错误码而非崩溃 | 样例 PE 直接加载运行，输出 `PE->CE roundtrip OK`，`exit(0)`，截图 02 |
| C. `tests/mkpe.py` | Python 工具把 PE32 翻译成 CE01 flat 结构；现场构造最小可运行 32 位 PE 样例并端到端验证 | 离线翻译出的 CE 在 guest 内运行成功，截图 04；`--selfcheck` 通过 |

## 2. 改动文件清单

### 2.1 内核源码（改动）

| 文件 | 改动内容 |
|---|---|
| `kernel_v25/src/task.c` | 新增 PE 结构体（`pe_dos_t` / `pe_coff_t` / `pe_section_t`）与常量；新增 `spawn_load_pe()`（PE32 直接加载：严格边界校验、段页映射合并、原始字节拷贝、入口范围校验；错误码 -5 坏格式 / -4 读盘 / -12 OOM / -3 过大）；新增 `pe_to_ce_file()`（内核侧把受支持 PE32 拍平成 CE01 写回 FAT 卷，带可读错误原因）；`task_spawn_ce()` 由 CE01/ELF32 双轨扩成三轨派发，PE 轨道不注入旧轨道专属的 image/bss/api 槽位 |
| `kernel_v25/include/task.h` | 声明 `pe_to_ce_file(const char *pe_path, const char *ce_path, char *err, int errsz)` |
| `kernel_v25/src/shell.c` | 新增 `g_script_mode`（autostart 脚本运行期间置位）；`cmd_run` 内 MZ 检测分支：交互时弹 y/n 转换询问，再问保存文件名（回车用默认）；非交互上下文（管道/重定向/后台/脚本）跳过询问；`boot_autostart` 循环前后置/清 `g_script_mode` |

### 2.2 测试与工具（新建）

| 文件 | 内容 |
|---|---|
| `tests/mkpe.py` | PE32→CE01 翻译器 + 最小 PE32 样例构造器。子命令：`--sample OUT.exe`、`IN.exe OUT.ce`、`--selfcheck`。CE 上限常量从 `kernel_v25/include/ce.h` 读取，避免与内核加载器漂移；与内核 `pe_to_ce_file()` 布局互为镜像 |
| `tests/p1_pe_check.py` | P1 端到端验收驱动：两遍 guest（pass1 format→注入，pass2 跑 A/B/C 三路径并截图）。自带本地 `/tmp` 暂存目录规避工作区 fs 的 QEMU image flock，阶段间拷回磁盘 |

### 2.3 文档（新建/追加）

| 文件 | 内容 |
|---|---|
| `docs/pe-dynamic-linking-design.md` | 轻量动态链接在 CatOS 上的可行性评估：现状 ABI、动态链接所需机制、三条路线（离线/装载期静态重定位 / 固定基址轻量共享模块 / 完整动态链接）成本与内核缺口、建议（不强求实现） |
| `docs/history.md` | 追加 P1 三条易错点（PE 段头 Name 字段错位、暂存盘跨遍丢失、多步交互询问漏答） |
| `docs/experience.md` | 追加 P1 三条经验（宿主侧先模拟校验、跨遍持久化磁盘模式、第三条二进制轨道的分派隔离） |

### 2.4 生成产物（可由源码再生，已纳入提交供离线复现）

| 文件 | 说明 |
|---|---|
| `tests/p1_artifacts/PESAMPLE.EXE` | `mkpe.py --sample` 构造的最小可运行 PE32 样例（1024 字节） |
| `tests/p1_artifacts/PESAMPLE.CE` | 离线翻译产物（536 字节，image=516，bss=0，api_off=0x200） |
| `tests/p1_shots/*.png` | 实机验收截图（见第 4 节） |

### 2.5 合并冲突预警（重点）

- **shell 命令表 `sh_cmds[]` 未改动**：没有新增、删除或改名任何 shell 命令；仅扩展了既有 `run` 命令在检测到 MZ 文件时的内部行为。主代理合并 `shell.c` 时无需对命令表做冲突处理。
- `task.c` 的改动集中在文件末尾 PE 段落 + `task_spawn_ce()` 分派处；`task.h` 仅追加一行声明。
- 未触碰 CE01 legacy 轨道与 ELF32 轨道的既有加载逻辑；三轨在分派点按魔数隔离。

## 3. 测试命令与结果

环境：QEMU 包装脚本 `/home/user/bin/qemu-system-i386`；启动参数 `-m 256 -no-reboot -net none -cdrom <iso> -boot order=d -display none -monitor tcp:127.0.0.1:4710`，纯 PS/2 键盘（未加 `-usbdevice keyboard`）。

### 3.1 构建

```
sh build.sh
# OK -> kernel_v25/catos.iso (11536384 字节)，编译/链接/打 ISO 无警告
```

### 3.2 工具自检

```
python3 tests/mkpe.py --selfcheck
# selfcheck OK: sample PE=1024 bytes -> CE=536 bytes (image=516 bss=0x0 api=0x200)
```

### 3.3 P1 端到端验收

```
python3 tests/p1_pe_check.py
# monitor connected (pass 1: format)
# inject OK PESAMPLE.EXE / PESAMPLE.CE
# monitor connected (pass 2: acceptance)
# PASS A: MZ convert prompt
# PASS B: direct PE32 run
# PASS C: mkpe.py CE runs
# PASS A: in-kernel conversion saved
# PASS A: converted CE runs
# steps=5 ok=5 fail=0
```

### 3.4 既有回归

```
python3 tests/ce_regress.py
# PASS=5 SKIP=13 FAIL=10
```
与改动前基线一致：10 条 FAIL 均为 `ce_sim` 未建模 opcode/API（`go.ce`、`heaptest.ce`、`mmaptest.ce`、`netinfo.ce`、`sndtest.ce` 等），属既有模拟器覆盖缺口，**非本次回归**。

```
python3 tests/qemu_regress.py kernel_v25/catos.iso --qemu /home/user/bin/qemu-system-i386 --port 4711
# steps=12 ok=11 warn=1
```
唯一 WARN 是 `boot-shell` 首帧亮度 0.01（shell 渲染前的轮询时序帧），后续 11 步（format/help/display/gfx/text N/Y/ls-bin/mmaptest/hello-elf/vdiag/final-frame）全部 PASS；该 WARN 是测试节奏敏感项，与本次改动无关。

## 4. 截图清单（绝对路径，已逐张 Read 复核内容）

1. `/home/user/Doubao/chats/38453665424926722/night/CatOs-P1/tests/p1_shots/01_mz_convert_prompt.png`
   `run /bin/PESAMPLE.EXE` → `'/bin/PESAMPLE.EXE' starts with MZ (PE32 binary). Convert to CE01 and save? (y/n)`
2. `/home/user/Doubao/chats/38453665424926722/night/CatOs-P1/tests/p1_shots/02_pe_direct_run.png`
   回答 n → `PE conversion skipped; trying direct load.` → `PE->CE roundtrip OK` → `[task 1 'PESAMPLE' exit(0)]`（路径 B 直接 PE 加载成功）
3. `/home/user/Doubao/chats/38453665424926722/night/CatOs-P1/tests/p1_shots/03b_convert_offer.png`
   再次 `run` 触发转换询问
4. `/home/user/Doubao/chats/38453665424926722/night/CatOs-P1/tests/p1_shots/03c_converted_ce_run.png`
   回答 y → `Save CE as [/bin/PESAMPLE.ce]:` → `converted to /bin/PESAMPLE.ce`（路径 A 内核转换保存成功）
5. `/home/user/Doubao/chats/38453665424926722/night/CatOs-P1/tests/p1_shots/03d_converted_ce_run.png`
   转换产物 `run /bin/PESAMPLE.CE` → `PE->CE roundtrip OK` → `exit(0)`（转换产物可运行）
6. `/home/user/Doubao/chats/38453665424926722/night/CatOs-P1/tests/p1_shots/04_offline_translated_ce_run.png`
   路径 C：`mkpe.py` 离线翻译的 CE → `PE->CE roundtrip OK` → `exit(0)`
7. `/home/user/Doubao/chats/38453665424926722/night/CatOs-P1/tests/p1_shots/05_ls_bin.png`
   `/bin` 目录列出 `pesample.exe (1024 bytes)`、`pesample.ce (536 bytes)`

## 5. 遗留问题与边界

- **导入/导出表被忽略**：当前 PE 只支持能独立运行、无外部导入符号的 freestanding EXE；依赖 DLL/API 调用的 Windows 程序无法运行（与设计文档路线 1 一致）。
- **未实现基址重定位（.reloc）**：`spawn_load_pe()` 按 PE 首选 ImageBase 映射；若镜像必须加载到别的基址，绝对引用不会被修正。内核直接加载路径要求 ImageBase 落在用户竞技场 `[0x2000000, 0x2800000)` 内。
- **CE 转换入口约束**：`pe_to_ce_file()` / `mkpe.py` 要求入口落在 flat 镜像偏移 0（CE 加载器总是从偏移 0 起跳）；入口不在镜像起始的布局会被拒绝并返回明确错误。
- **无共享库/模块动态装载**：多程序复用同一份库代码需要的共享只读页、模块登记表、符号查询接口均未实现，评估见 `docs/pe-dynamic-linking-design.md`。
- **转换器并发**：`pe_ce_bounce` 为静态缓冲，面向单 shell 会话；未考虑并发转换（当前 shell 串行执行，无影响）。
- **非交互行为**：管道/重定向/后台/autostart 脚本中跳过转换询问，提示一行后继续走直接加载路径；不会阻塞在键盘 readline。

## 6. 复现步骤

```
cd /home/user/Doubao/chats/38453665424926722/night/CatOs-P1
sh build.sh
python3 tests/mkpe.py --selfcheck
python3 tests/ce_regress.py
python3 tests/qemu_regress.py kernel_v25/catos.iso --qemu /home/user/bin/qemu-system-i386 --port 4711
python3 tests/p1_pe_check.py
```

## 7. 全部改动文件列表（供主代理合并核对）

源码：
- `kernel_v25/src/task.c`（改）
- `kernel_v25/include/task.h`（改）
- `kernel_v25/src/shell.c`（改；`sh_cmds[]` 命令表未改）

测试/工具：
- `tests/mkpe.py`（新）
- `tests/p1_pe_check.py`（新）

文档：
- `docs/pe-dynamic-linking-design.md`（新）
- `docs/history.md`（追加）
- `docs/experience.md`（追加）

生成产物（可再生）：
- `tests/p1_artifacts/PESAMPLE.EXE`
- `tests/p1_artifacts/PESAMPLE.CE`
- `tests/p1_shots/01_mz_convert_prompt.png`
- `tests/p1_shots/02_pe_direct_run.png`
- `tests/p1_shots/03b_convert_offer.png`
- `tests/p1_shots/03c_converted_ce_run.png`
- `tests/p1_shots/03d_converted_ce_run.png`
- `tests/p1_shots/04_offline_translated_ce_run.png`
- `tests/p1_shots/05_ls_bin.png`

未纳入提交的运行期文件：`tests/p1_run.log`、`tests/p1_qemu.log`（`*.log` 已被 .gitignore 忽略）；磁盘镜像 `tests/qemu_disk_p1.img`、`/tmp/catos_p1_stage/` 为临时产物。
