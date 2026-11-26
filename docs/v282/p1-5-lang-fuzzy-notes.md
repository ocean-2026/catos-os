# P1-5（B5）· 汉化 key 查表 + 模糊回退 — 验收记录

分支：`v282/p1-4-5-6-cde-lang-detach`，基线 `fbab3a7`。

## 1. 做了什么

`kernel_v25/src/lang.c` 的 `trhelp()` 在**保持精确匹配优先、已有精确表行为不变**的前提下，加了两级回退：

1. **精确匹配**（原逻辑，逐字节比较，最先命中即返回）；
2. **归一化匹配**：大小写折叠、首尾空白裁剪、内部空白串压成单空格、丢弃尾部标点（`. , ; : ! ? ) ]`），内部标点保留以免不同条目互相吞并；
3. **首英文词松散回退**：取输入与表项的首个英文词比较；新增 `trhelp_cmd(cmd, en)` 把命令名作为**最佳努力**的消歧提示——首词命中多条时，优先返回描述里提到该命令 token 的条目；表本身只存描述、不存命令名，所以命令名只能偏好、不能强制选行。

改动文件：

- `kernel_v25/src/lang.c`：新增归一化 / 首词 / 大小写不敏感包含等本地辅助函数；`trhelp()` 改为 `trhelp_cmd(0, en)` 的薄包装；`trs()` 未改动。
- `kernel_v25/include/lang.h`：声明 `trhelp_cmd()`。
- `kernel_v25/src/shell.c`：`help_row()` 改调 `trhelp_cmd(cmd, desc)`，把命令名带入松散回退；未触碰命令分发与管道解析。
- `tests/lang_fuzzy_probe.py`（新增）：三段式可复跑探针（静态覆盖 / 主机编译实码 / QEMU 实机冒烟）。

## 2. 验证命令与真实输出

### 2.1 构建与护栏

```
$ cd kernel_v25 && sh build.sh iso
OK -> .../kernel_v25/catos.iso (12263424 bytes)        RC=0

$ python3 tools/check_link_layout.py build/kernel.pe --strict
GUARD_RC=0
```

### 2.2 静态覆盖（解析真实 `shell.c` 字面量 vs 真实 `help_zh[]` 表）

```
$ python3 tests/lang_fuzzy_probe.py
static: 121 help strings, table entries=168
static: exact=115 normalised=0 loose-first-word=2 unmapped=4
  unmapped: [row] desktop -- enter the block desktop (Esc returns to console)
  unmapped: [row] perf [on|off|reset] -- GUI frame timing / damage perf stats
  unmapped: [row] gui [idem|leak] -- block declarative GUI desktop (Esc exits); idem/leak = serial probes
  unmapped: [row] fpu -- FPU capability / CR0-CR4 / save-restore roundtrip
PASS static exact hits >= 115           exact=115
PASS static unmapped set unchanged (4 pre-existing) unmapped=4
```

口径说明：任务书提到的“63 项”是 v2.75 最初全量汉化时的命令数；当前 `shell.c` 实际可翻译串为 **121** 条（106 条 `help_row` + 9 条 `help_head` + 6 条 `help_note`），表内 168 条。精确命中 **115**，归一化新增命中 0（现有源码串本身没有空白/大小写/标点噪声），首词松散命中 2，剩余 **4 条为基线 `fbab3a7` 就未建表的既有英文回退**，本轮未新增也未丢失。

### 2.3 主机编译实码（真实 `lang.c` + fs/hive 桩，非 Python 复刻）

探针在临时目录用 gcc 编译内核 `lang.c`，直接调 `trhelp()`：

```
PASS exact baseline                 got=显示本帮助 want=显示本帮助
PASS variant multi-space            got=显示本帮助 want=显示本帮助
PASS variant trailing dot           got=显示本帮助 want=显示本帮助
PASS variant case                   got=显示本帮助 want=显示本帮助
PASS heading multi-space            got=网络（PCnet） want=网络（PCnet）
PASS heading case+punct             got=网络（PCnet） want=网络（PCnet）
PASS unknown -> English             got=zzz no such entry qqx want=zzz no such entry qqx
PASS en passthrough                 got=show this message want=show this message

ALL PASS
PASS host harness all PASS              8 PASS, rc=0
```

三条构造变体全部命中：
- 多空格：`"   show   this   message   "` → `显示本帮助`
- 尾随标点：`"show this message."` → `显示本帮助`
- 大小写：`"Show This Message"` → `显示本帮助`

### 2.4 QEMU 实机冒烟

```
$ python3 tests/lang_fuzzy_probe.py
PASS help under zh: no fault
PASS help under zh: command names still English
PASS keys under zh: no fault
PASS help under en: no fault
PASS help under en: English description visible

8/8 PASS        RC=0
```

截图：`artifacts/v282/p1-5/01-zh-help.png`、`02-keys-zh.png`、`03-en-help.png`；落屏文本 `zh_help_screen.txt` / `en_help_screen.txt`。

### 2.5 门禁

```
$ python3 tests/e3_regress.py kernel_v25/catos.iso
PASS badptr / PASS conc        steps=2 ok=2 fail=0

$ python3 tests/vprobe_regress.py kernel_v25/catos.iso
steps=10 ok=10 fail=0
```

## 3. 未做 / 降级项及客观理由

- **4 条既有未建表描述**（desktop / perf / gui / fpu）仍原样英文回退：本轮要求“不改已有精确表行为”，未补表项；松散回退也未强行猜译，避免错配。
- **实机无法回读中文文本**：text 模式 VGA 字库无中文字形（见 P1-3 / 问题清单），`xp` 文本缓冲里非 ASCII 单元被读成空白，因此实机侧只能断言“无崩溃、命令名仍英文、`keys` 不报错、切回 en 后英文描述可见”；中文是否命中由 2.2 静态对照表 + 2.3 真实代码主机编译运行共同保证。
- **命令名消歧是最佳努力**：`help_zh[]` 表项不带命令名字段；首词命中多条时，命令名只用于偏好排序，无法像正式 key 一样唯一确定行。
- `trs(en, zh)` 内联双语挑选器未加模糊逻辑：它的两个参数由调用点成对给出，不存在查表失配场景。

## 4. 新发现的限制 / 坑（供问题清单汇总）

- 崩溃判定关键字在探针里容易误报：`faultinfo`（命令名）、`panic record`（crashlog 描述）都是正常文本；真实崩溃横幅是 `!!! PANIC !!!`（`bsod.c:298`），故障标记为 page/kernel fault、system halted。
- text 模式无法从 VGA 文本缓冲回读 CJK 字节，汉化类验收必须配合静态对照表或 gfx 截图，不能只靠 `screen_text()`。
