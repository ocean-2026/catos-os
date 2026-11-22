# help 中文汉化待办

> 状态：**待办**（尚未修改任何代码）
> 建立日期：2026-11-22
> 依据：`kernel_v25/src/shell.c:cmd_help()`（第 278–411 行）与
> `kernel_v25/src/lang.c:help_zh[]`（第 114–220 行）逐条比对。
> 比对方法：提取所有 `help_row` 的描述列、`help_head`、`help_note`、`pager_line`
> 的字符串字面量，与 `help_zh[].en` 求差集；脚本结果：
> `missing=63`（62 条描述 + 1 条标题）, `covered=52`，`help_zh` 现有 102 个键。

---

## 1. 背景与机制

`lang zh` 下 `help` 的汉化由 `trhelp()`（`lang.c:229`）完成：

- `help_row(cmd, desc)`：`cmd`（命令名/参数）**保持英文**，`desc` 经 `trhelp()` 翻译；
- `help_head(s)` / `help_note(s)`：整行经 `trhelp()` 翻译；
- 未命中 `help_zh[]` 的字符串**原样回退英文**——所以缺表项不会报错，只是显示英文；
- `help_zh[]` 是大小写敏感的精确整串匹配（`help_zh_cmp`）；措辞差一个词、一个括号
  都会漏，例如表里是 `remove an empty directory`，而 help 用的是
  `remove an empty directory (was rmdir)` → 判为未翻译。

**设计约定（不要误改）**：命令名、参数名、CXX 选项、`/etc/xxx` 路径、CAP_ 能力名、
信号名属"脚本接口面"，按原设计保持英文。待办只针对**人类可读的描述列与标题**。

**一个代码缺陷**：`shell.c:281` 的标题串 `pager_line("CatOS v0.3 (five-pillar kernel) - commands:")`
是**直接输出**，根本没经过 `trhelp()`。要汉化它必须改 `shell.c`（改成
`pager_line(trhelp("..."))` 并补表项），只往 `help_zh[]` 加键无效。

---

## 2. 待办清单（62 条描述 + 1 条标题 = 63 项）

下面每项均是 **英文原文 → 建议中文译文**。译文为建议稿，落地时可微调。

### 2.0 基础区（3 项）

| # | 命令 | 英文原文 | 建议译文 |
|---|------|----------|----------|
| 1 | sndregs | raw ES1370/ES1371 register readback | 读取 ES1370/ES1371 寄存器原始值 |
| 2 | fprobe | paint the whole frame + 4 corner marks (coverage probe) | 整屏填色 + 四角标记（覆盖率探测） |
| 3 | sndmap | read-only dump of all ES137x registers 0x00..0x3C | 只读转储 ES137x 全部寄存器 0x00..0x3C |

### 2.1 fun 彩蛋（5 项）

| # | 命令 | 英文原文 | 建议译文 |
|---|------|----------|----------|
| 4 | cowsay | ASCII cow (or Tux) says MSG | ASCII 奶牛（或 Tux）说出 MSG |
| 5 | sl | steam train drives by (CHOO CHOO) | 蒸汽火车驶过（呜——） |
| 6 | fortune | print a random fortune | 输出一条随机箴言 |
| 7 | cal | month calendar, or full year with YEAR | 当月日历；带 YEAR 则输出全年 |
| 8 | yes | repeat TEXT until Ctrl+C | 重复输出 TEXT 直到 Ctrl+C |

### 2.2 display 显示（3 项）

| # | 命令 | 英文原文 | 建议译文 |
|---|------|----------|----------|
| 9 | gfx | `display mode switcher + diagnostics:` 及后续 4 行 MODE/CMD 说明 | 显示模式切换 + 诊断：\n     MODE = gfx \| vbe \| gfx24\n     CMD  = svgainfo \| size\n          \| rgb FG [BG] \| palette IDX RRGGBB \| display |
| 10 | style | bold+italic+underline / style fg #RRGGBB\|canvas / style bg surfaceRaised | 粗体+斜体+下划线 / `style fg #RRGGBB\|canvas` / `style bg surfaceRaised` |
| 11 | vdiag | raw VGA/VBE register diagnostic | VGA/VBE 寄存器原始值诊断 |

### 2.3 Chinese 中文区（5 项）

| # | 命令 | 英文原文 | 建议译文 |
|---|------|----------|----------|
| 12 | zhdemo | built-in UTF-8 Chinese + truecolour demo | 内置 UTF-8 中文 + 真彩演示 |
| 13 | reboot | warm reboot the machine | 热重启机器 |
| 14 | hang | halt the CPU (QEMU: close window) | 挂起 CPU（QEMU 中请关闭窗口） |
| 15 | shutdown | power off via ACPI PM1_CNT (root/CAP_SYS; honest reboot fallback) | 通过 ACPI PM1_CNT 关机（需 root/CAP_SYS；失败则如实回退为重启） |
| 16 | bsod\|blue | render the full-screen crash report (--no-reboot for test) | 渲染全屏崩溃报告（测试用 `--no-reboot`） |

### 2.4 processes 进程（2 项）

| # | 命令 | 英文原文 | 建议译文 |
|---|------|----------|----------|
| 17 | run | execute a CE app (in background with &) | 执行 CE 程序（加 `&` 则后台运行） |
| 18 | sudo | run a program as root (uid 0) | 以 root（uid 0）运行程序 |

### 2.5 users 用户与权限（12 项）

| # | 命令 | 英文原文 | 建议译文 |
|---|------|----------|----------|
| 19 | id | uid/gid/capabilities of a user | 用户的 uid/gid/能力集 |
| 20 | caps | capability bit legend + session caps | 能力位图例 + 当前会话能力 |
| 21 | login | switch session user ('su' = alias) | 切换会话用户（`su` 为别名） |
| 22 | useradd | add user (CAP_USERADM) | 添加用户（需 CAP_USERADM） |
| 23 | groupadd | create group / append member (CAP_USERADM) | 创建组 / 追加成员（需 CAP_USERADM） |
| 24 | daemon | detach a service as a background daemon | 将服务分离为后台守护进程 |
| 25 | groups | list groups and members (v2.53 /etc/group) | 列出组及成员（/etc/group） |
| 26 | userdel… | manage accounts / caps | 管理账户 / 能力 |
| 27 | chmod… | persistent /etc/acl | 持久化到 /etc/acl |
| 28 | top | live task monitor (CPU% per task, any key quits) | 实时任务监视器（各任务 CPU%，任意键退出） |
| 29 | log | show a background job's captured output | 查看后台任务捕获的输出 |
| 30 | log -f | follow that job until it exits | 持续跟踪该任务直到退出 |

### 2.6 作业控制 / 环境（4 项）

| # | 命令 | 英文原文 | 建议译文 |
|---|------|----------|----------|
| 31 | fg | resume a stopped/background job in foreground | 将停止/后台任务转入前台继续运行 |
| 32 | bg | resume a stopped job in background | 将停止的任务转入后台继续运行 |
| 33 | stop | suspend a running job (SIGTSTP; same as Ctrl+Z) | 挂起运行中的任务（SIGTSTP，等同 Ctrl+Z） |
| 34 | env | set variable (empty V deletes); env lists all | 设置变量（V 为空则删除）；`env` 列出全部 |

### 2.7 environment 环境（2 项）

| # | 命令 | 英文原文 | 建议译文 |
|---|------|----------|----------|
| 35 | unset | delete variable; $K / ${K} / $? expand in typed lines | 删除变量；键入行中 `$K` / `${K}` / `$?` 会展开 |
| 36 | hwinfo | full hardware dump (CPU/mem/disk/NIC/PCI) | 完整硬件信息（CPU/内存/磁盘/网卡/PCI） |

### 2.8 audio 音频（标题 1 + 2 项）

| # | 命令 | 英文原文 | 建议译文 |
|---|------|----------|----------|
| 37 | — (help_head) | audio (SB16 ISA / ES1370-1371 PCI, POSIX options) | 音频（SB16 ISA / ES1370-1371 PCI，POSIX 选项） |
| 38 | sndinfo | audio info: -h -v -l -p -c -d N -w -j -s -q -t (long forms too) | 音频信息：`-h -v -l -p -c -d N -w -j -s -q -t`（也支持长选项） |
| 39 | sndtest | play a tone (default 440 Hz / 800 ms) | 播放测试音（默认 440 Hz / 800 ms） |

### 2.9 network 网络（5 项）

| # | 命令 | 英文原文 | 建议译文 |
|---|------|----------|----------|
| 40 | curl | HTTP GET (http://IP[:port]/path) | HTTP GET（`http://IP[:port]/path`） |
| 41 | ping | ICMP echo (3 tries) | ICMP 回显（3 次） |
| 42 | dns | resolve a hostname | 解析主机名 |
| 43 | udp | send one UDP datagram and wait for a reply | 发送一个 UDP 数据报并等待回复 |
| 44 | netdiag | dump NIC/driver state | 转储网卡/驱动状态 |

### 2.10 filesystem 文件系统（18 项）

| # | 命令 | 英文原文 | 建议译文 |
|---|------|----------|----------|
| 45 | ls | list: -l long, -t/-lt tree, --color-scale, --blocks=COLS | 列出：`-l` 详细、`-t`/`-lt` 树形、`--color-scale`、`--blocks=COLS` |
| 46 | rm -d | remove an empty directory (was rmdir) | 删除空目录（原 `rmdir`） |
| 47 | pkg … | package manager (/pkg + remote HTTP) | 包管理器（/pkg + 远程 HTTP） |
| 48 | pkg repo | set/show remote package repository base URL | 设置/查看远程软件仓库根 URL |
| 49 | pkg update | download <repo>/PKGS.TXT into local index | 下载 <repo>/PKGS.TXT 到本地索引 |
| 50 | pkg fetch | download + verify + install a signed .catpkg | 下载 + 校验 + 安装签名的 .catpkg |
| 51 | head | print first N lines (default 10) | 输出前 N 行（默认 10） |
| 52 | tail | print last N lines (default 10) | 输出后 N 行（默认 10） |
| 53 | wc | line/word/byte count | 行/词/字节计数 |
| 54 | tree | directory tree (alias: ls -t; -lt adds sizes) | 目录树（`ls -t` 的别名；`-lt` 附带大小） |
| 55 | format | erase the FAT32 volume (y/n confirm) | 擦除 FAT32 卷（需 y/n 确认） |
| 56 | fsck | check FAT mirror + scan dir tree; --purge wipes bad entries | 检查 FAT 镜像 + 扫描目录树；`--purge` 清除坏项 |
| 57 | mount | list the mount table | 列出挂载表 |
| 58 | mount … | bind a FAT32 partition to an empty dir | 把 FAT32 分区绑定到空目录 |
| 59 | umount | detach the partition bound to DIR | 卸载绑定到 DIR 的分区 |
| 60 | df | per-mountpoint volume usage | 各挂载点的卷使用情况 |
| 61 | faultinfo | show the last fatal fault (vector, CR2, PF bits) | 显示最近一次致命错误（向量、CR2、缺页标志位） |
| 62 | crashlog | show the saved panic record (crashlog --full for raw) | 显示保存的崩溃记录（`crashlog --full` 看原始内容） |

### 2.11 单独一项：标题（需改代码，不在 help_zh 表）

| 位置 | 英文原文 | 处理办法 |
|------|----------|----------|
| `shell.c:281` | `CatOS v0.3 (five-pillar kernel) - commands:` | 把 `pager_line("…")` 改为 `pager_line(trhelp("…"))`，并在 `help_zh[]` 加 `"CatOS v0.3 (five-pillar kernel) - commands:"` → `"CatOS v0.3（五柱内核）—— 命令列表："` |

---

## 3. 落地步骤（建议）

1. **补表**：把 §2 的 62 条描述（加 §2.11 标题共 63 条）英文原文作为 `help_zh[]` 的 `en` 键、建议译文作为 `zh` 值
   追加到 `lang.c:114` 的数组末尾（保持 `{"...", "..."},` 格式，注意 C 字符串里的
   反斜杠与竖线要正确转义）。
2. **改标题**：按 §2.11 改 `shell.c:281`。
3. **注意重复键**：`help_zh[]` 里已有 `remove an empty directory`，而 help 用的是
   `remove an empty directory (was rmdir)`——两者都要保留（前者可能服务其它调用点）。
4. **验证**：
   - 用 `python` 重跑 §开头描述的差集脚本，确认 `missing` 降到 0（标题那条改为
     检查 `trhelp` 包装后计入）；
   - QEMU 实机：`lang zh` → `help`，用 HMP `screendump` 截图，放大后逐组核对；
     注意 gfx 模式下 `screen_text()` 读的是 TEXT buffer，验证前先 `text` + 物理 `y`
     切回文本模式。
5. **回归**：`help` 英文模式输出不变（`trhelp` 在 `LANG_EN` 下恒返回原文）。

---

## 4. 附带发现（非 help 命令）

`cmd_keys()`（`shell.c:417`）同样走 `trhelp()`，其数据源 `keys_table.c` 的字符串里，
除键名（Left/Right、Esc、Enter、Ctrl+C 等，按设计保留英文）外，还有一条描述未翻译：

- `deletes pinyin letter while IME active` → 建议"输入法开启时删除拼音字符"

如要一并汉化，追加到 `help_zh[]` 即可（它已是 keys 与 help 共享的翻译表）。

---

## 5. 相关文件

- `kernel_v25/src/shell.c` — `cmd_help()` / `cmd_keys()` / `help_row/head/note`
- `kernel_v25/src/lang.c` — `help_zh[]` / `trhelp()` / `g_lang`
- `kernel_v25/src/keys_table.c` — `keys` 命令的数据源
- `kernel_v25/include/lang.h` — `LANG_EN` / `LANG_ZH` / `lang_name()`
