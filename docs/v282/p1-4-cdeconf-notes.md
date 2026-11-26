# P1-4（A1.4）· `/etc/cde.conf` 外观配置镜像 — 复核与验收记录

分支：`v282/p1-4-5-6-cde-lang-detach`，基线 `fbab3a7`。

## 1. 做了什么

新增内核态模块 `cdeconf`，在既有持久化范式（`/etc/style`、`/etc/lang`、`/etc/env` 等一行一键值文本文件）之上，把终端外观方案镜像到 `/etc/cde.conf`。**没有新增 CE ABI 槽、没有新增 syscall、没有新增 shell 命令。**

改动文件：

- `kernel_v25/include/cdeconf.h`（新增）：接口 `cdeconf_load()` / `cdeconf_save_theme()` / `cdeconf_scheme()`，文件格式与优先级注释。
- `kernel_v25/src/cdeconf.c`（新增）：INI 解析（`#` 注释、`KEY=VALUE`、首尾空白裁剪、大小写不敏感 key），`theme` / `accent` 两个生效键；`fontsize` / `taskbar` 为保留键，解析但不应用。
- `kernel_v25/build.sh`：把 `cdeconf.c` 加入编译列表与两处链接对象列表。
- `kernel_v25/src/shell.c`：
  - shell 初始化 `pal_init()` 之后调用 `cdeconf_load()`（缺文件 / 卷未就绪 / 键非法时回落到编译期默认，不崩）。
  - `cmd_theme()` 切换方案后调用 `cdeconf_save_theme(name)` 写回镜像。
  - 仅含这两处 theme 相关接线；管道解析行未触碰。
- `kernel_v25/src/gui/chrome.c`：`gui_demo_build()` 里读 `cdeconf_scheme()`，仅 `light` 映射到 `TOK_LIGHT`，其余回落 `TOK_DARK`（块化桌面没有 solarized/mono 的独立 gui token）。
- `tests/cdeconf_probe.py`（新增）：三引导实机探针。

### 文件格式（`/etc/cde.conf`）

```
# CatOS appearance mirror (v2.82 A1.4); hive stays primary.
theme=light
accent=#8250DF
```

优先级：hive（`HKLM\Software\CatOS\Terminal\Theme`）仍是主源；只有 hive 无方案标签（含标签为 `custom` 视为未标注）时，才采纳文件里的 fallback 值。启动不会主动写文件。

## 2. 验证命令与真实输出

工作目录：`work/v282-p14p15p16/kernel_v25`

### 2.1 构建

```
$ sh build.sh iso
```
末行：`OK -> .../kernel_v25/catos.iso (12259328 bytes)`，**RC=0**。

### 2.2 链接护栏

```
$ python3 tools/check_link_layout.py build/kernel.pe --strict
```
**RC=0**（仅有既存 arena-window 静态表告警，与基线同类，无新增报错）。

### 2.3 实机探针（三引导，固定盘副本 + 全新格式化盘）

```
$ python3 tests/cdeconf_probe.py
```

真实输出关键行：

```
PASS theme light applied (bg F6F6F4)    scheme=light bg=F6F6F4
PASS cde.conf written with theme=light
PASS cde.conf carries live accent mirror  accent=#8250DF
PASS theme still light after reboot     scheme=light bg=F6F6F4
PASS cde.conf survived reboot
PASS shell clean after rm cde.conf
PASS fresh volume boots to default dark scheme=dark bg=0A0E14

7/7 PASS
```

各引导落盘文本（`artifacts/v282/p1-4/`）：

| 场景 | 文件 | 关键实测值 |
| --- | --- | --- |
| 改主题前 | `boot1_theme_before.txt` | `current theme: dark background #0A0E14` |
| 改 `theme light` 后 | `boot1_cdeconf.txt` | `theme=light accent=#8250DF`；`theme` 报 `light #F6F6F4` |
| 重启后 | `boot2_theme_after.txt` | `current theme: light background #F6F6F4`（方案跨重启保持） |
| 删文件后 | `04-after-rm.png` + 探针判词 | shell 干净回到 prompt，无 page fault / halted |
| 全新格式化盘 | `boot3_fresh_default.txt` | `current theme: dark background #0A0E14`（缺文件回默认） |

截图：`artifacts/v282/p1-4/01-before-theme.png`、`02-light-applied.png`、`03-after-reboot.png`、`04-after-rm.png`、`05-fresh-default.png`。

### 2.4 门禁

```
$ python3 tests/e3_regress.py kernel_v25/catos.iso
PASS badptr       verdict PASS
PASS conc         verdict PASS
steps=2 ok=2 fail=0        E3_RC=0

$ python3 tests/vprobe_regress.py kernel_v25/catos.iso
PASS format / bin-has-vprobe / fr / cat / hw / str / strcpy / wr / fw / pw
steps=10 ok=10 fail=0       VP_RC=0
```

## 3. 未实现 / 未应用的字段（客观理由，无伪造读写）

任务书列了四个字段：theme、强调色、字号、任务栏位置。

| 字段 | 状态 | 客观理由 |
| --- | --- | --- |
| `theme` | 已实现 | INI `theme=` 读取并 `pal_apply_scheme()`；`theme` 命令写回；跨重启保持（2.3 实测）。 |
| `accent`（强调色） | 已实现（镜像） | `#RRGGBB` 解析后 `pal_set_rgb(PAL_ACCENT, rgb)`；写回时保留既有 accent 行，否则镜像当前 PAL_ACCENT。仅在 hive 未标注方案时采纳。 |
| `fontsize` | **解析保留，未应用** | 块化桌面（`kernel_v25/src/gui/`）当前没有运行时字号状态：渲染走 8×8 位图字模（`render_font.c` / `font8.c`），没有可切换的字号档位。文件里出现 `fontsize=` 不会报错，但不改变任何已渲染字号。 |
| `taskbar`（任务栏位置） | **解析保留，未应用** | 块化桌面的任务栏在 `chrome.c` / `sb.c` 里固定在下边缘，没有上/左/右的布局参数；文件里出现 `taskbar=` 不会报错，但不改变任务栏位置。 |

补充偏差：任务书写的落点是 `desk.ce`（`extapp/desk`）。实际镜像里该 extapp 不存在，桌面在 `kernel_v25/src/gui/` 内核态；本模块因此直接挂到 shell 初始化与 gui 构建入口，未复用 `save`/`cat` 用户态 ABI。

另外：`solarized` / `mono` 两个方案在 shell palette 层可用，但 gui token 层只有 `TOK_LIGHT` / `TOK_DARK`，进桌面时这两个方案回落 `TOK_DARK`，无独立 gui 配色。

## 4. 新发现的限制 / 坑（供问题清单汇总）

- 块化桌面缺运行时字号档位与任务栏位置布局参数，外观配置能落盘的只有 theme / accent 两项；`fontsize` / `taskbar` 需要先在 gui 层补状态模型，再回来接线。
- `/etc/cde.conf` 是 hive 之外的 legacy mirror；hive 有标签时文件被忽略，文档与用户预期需要说明这一优先级。
