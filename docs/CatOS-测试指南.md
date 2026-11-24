# CatOS 测试指南（系统内手工测试 · v2.78 基线）

> 2026-11-23 更新：基线从 v2.59 升到 **v2.78**；新增 §A 基线差异、§B v2.60–v2.78 新能力用例、§C 自动化套件索引、§D 判定陷阱。
> v2.59 的原始全量用例保留在**附录**，未改动（仍有效，命令面只增不减）。
> 配套：宿主侧自动化见 `tests/`（115 个脚本），入口 `tests/run_full_regress.sh`；键位见 `设计手册.md` 第四部分；待办/缺陷见 `项目总纲.md` 第三部分与 `CatOS-内核问题清单.md`。

## 0. 怎么用这份文档

1. **冒烟**（10 分钟）：按 §B-0 跑一遍，先确认引导、提示符、`ls`、`run hello.ce`、`faultinfo` 正常。
2. **回归新能力**：按 §B 的主题表逐组跑，每组记 PASS/FAIL（FAIL 四要素见 §D.1）。
3. **怀疑旧功能**：查附录（v2.59 全量 14 组用例）。
4. **只信实机**：编译通过不算完成；`screen_text()` 在 gfx 下读不到内容，必须截图（§D.2）。

## A. 基线差异：v2.59 → v2.78

| 项 | v2.59（原指南写作时） | v2.78（当前） |
|---|---|---|
| 引导镜像 | `catos-v2.59.iso` | **`catos-v2.78.iso`**（仓库根目录；同目录另有多个历史 ISO，别拿错） |
| 数据盘 | `kernel_v25/catos_disk.img` | 同左；干净盘可用 `catos_disk-v2.78-fresh.img` |
| CE 槽数 | 65 | **69**（66–68 鼠标原语，69 `task_get`） |
| 图形 | gfx24 800×600 24bpp | 同左；VMware 走 SVGA II（32bpp XRGB 承载 RGB24） |
| 网络 | PCnet 驱动 + `curl` 明文 | + e1000、DHCP、`pkg` 远程仓库、**TLS `https://`**、**sshd** |
| 包管理 | 本地 `/pkg/` | + `pkg repo/update/fetch`；SHA-256 截断签名；流式下载（解除包体上限） |
| 桌面 | 无 | `desktop.ce`（CDE P1）经 `pkg install/remove desktop` 装卸 |
| 中文 | IME 单音节 + 一行候选 | 两行候选区；`lang zh` 下 `help` 63 项 + keys 1 条已汉化（v2.75） |
| 终端 | 回溯 96 行 | 回溯 **10000 行**；任意键跳过滚屏；滚轮/PgUp 单次重绘（v2.76） |
| 回归 | 22 套件 / 270 断言 | `tests/` **115 个脚本**，按主题索引见 §C |

## B. v2.60–v2.78 新增能力用例

> 依据：`历史记录.md` 第一部分（各版本变更说明）与 `项目总纲.md` 第三部分。**以下是待执行用例，不是实测结论**。

### B-0 冒烟（必过，5 项）

| # | 操作 | 预期 |
|---|---|---|
| B0-1 | 引导 `catos-v2.78.iso`（+数据盘） | 出 `root@catos:/$`，banner 版本行 v2.78 |
| B0-2 | `hwinfo` | CPU/Mem/Disk/NIC 四项有值，`cores 1`（无 SMP） |
| B0-3 | `ls /bin` | CE 应用齐全（≥30 个 `.ce`） |
| B0-4 | `run hello.ce` | 打印欢迎行并回提示符，退出码 0 |
| B0-5 | `faultinfo` / `crashlog` | 无新故障记录（或显示上次记录，不 panic） |

### B-1 终端与显示

| # | 操作 | 预期 | PASS 要点 |
|---|---|---|---|
| B1-1 | `videomode` 切 gfx24，重启 | 模式持久化（注册表），重启后仍是 gfx24 | 重启后 `svgainfo` 的宽高不变 |
| B1-2 | `colors set <role> RRGGBB` → 重启 | 配色持久；不被开机主题覆盖 | 重启后同色 |
| B1-3 | `fprobe` | 整屏填色 + 四角标记 | 无残角、无半屏旧色（v2.72 修复点） |
| B1-4 | 长输出中按任意键 | 立即中止剩余输出并回提示符，按键被行编辑器接收 | 不吞键；`help > f` 重定向时不跳 |
| B1-5 | 滚轮 / PgUp 连续翻页 | 单次事件一次重绘，无逐行闪烁 | 1 万行历史可翻到顶 |
| B1-6 | `style fg #RRGGBB` / `size` 三档 | 字号/颜色即时生效 | 历史行按调色板还原（自定义 RGB 显示为近似色） |

### B-2 文件系统与包管理

| # | 操作 | 预期 |
|---|---|---|
| B2-1 | `pkg repo http://<host>:<port>/` → `pkg update` | 拉取 `PKGS.TXT` 成本地索引，`pkg list` 可见 |
| B2-2 | `pkg fetch <name>` | 下载 + 验签 + 安装到 `/bin`，命令可用 |
| B2-3 | 篡改包/旧摘要包 | 安装被拒（签名校验失败），系统不崩 |
| B2-4 | 下载 ≥46 KiB 的包（CL 与 chunked 两种） | 流式落盘到 `/tmp/.pkgdl` 后分片校验，成功安装 |
| B2-5 | 三层嵌套目录 `rm -r` | 全部删除；`fsck` 报 FAT mirror OK / directory tree OK |
| B2-6 | `format`（专用测试盘） | y/n 确认后重建卷；`/bin` 清空（**非专用盘禁止**） |

### B-3 网络

| # | 操作 | 预期 |
|---|---|---|
| B3-1 | `netcfg 10.0.2.15 10.0.2.2 10.0.2.3`（QEMU user） | `netinfo` 显示 IP/网关/DNS |
| B3-2 | `curl http://10.0.2.2:8000/x.txt` | 取回文本；跟随 3xx、解 chunked、无 scheme 也接受 |
| B3-3 | `curl https://10.0.2.2:8443/`（`openssl s_server -tls1_2 -cipher ECDHE-RSA-AES128-SHA256`） | 取回固定串（v2.78 验收串 `CATOS_TLS_OK_278`）；**仅加密未验签**，不可当安全通道 |
| B3-4 | `ping` / `dns` / `udp` | 分别 3 次回显、解析成功、UDP 有回包 |
| B3-5 | 外部 `ssh root@<guest-ip>`（OpenSSH/paramiko） | 可登录到 CatOS shell；重启后 host key 不变（`@sshd` 自启动） |

### B-4 桌面（CDE P1，可拆卸）

| # | 操作 | 预期 |
|---|---|---|
| B4-1 | `pkg install desktop` → `desktop` | 进入桌面；鼠标可移动/点击；`q` 退出回 shell（v2.73 修复点） |
| B4-2 | 桌面下键入普通字符 | 键盘事件不被 sshd 守护任务阻塞（v2.73 修复点） |
| B4-3 | `pkg remove desktop` | 恢复纯 shell；不残留桌面进程 |
| B4-4 | 桌面上滚轮/拖拽 | 指针不跳变（v2.72 滚轮 ACK/ID 错位已修） |

### B-5 中文与输入法

| # | 操作 | 预期 |
|---|---|---|
| B5-1 | `lang zh` → `help` | 63 项描述 + 标题中文，命令名保持英文 |
| B5-2 | `lang zh` → `keys` | 键位说明中文，键名英文（1 条已补） |
| B5-3 | gfx 下 F2 开 IME，输入拼音 | 两行候选区出现在输入行下方；数字/Space/`-`/`=`/Esc 生效 |
| B5-4 | 输出滚动时 IME 打开 | 候选框被摘除后钉回新底部，不随内容卷走（v2.77） |
| B5-5 | 输入 `。，、` | 显示为中文标点（各占 1 格），不再出现两个 `?` |
| B5-6 | `text` 模式下 `cat` 中文文件 | 中文字形缺失（已知限制 #117，**非缺陷**） |

### B-6 电源与诊断

| # | 操作 | 预期 |
|---|---|---|
| B6-1 | `shutdown`（root/CAP_SYS） | 先 ACPI enable 再写 `\_S5`；VMware 真机应关机而非重启（待 G1 复测） |
| B6-2 | `bsod --no-reboot` | 全屏崩溃报告，含向量/ring/任务/eip/CR2；标题字模缺字处降级 ASCII（#202） |
| B6-3 | 触发 #PF 的 CE（`crash.ce`） | 只杀该任务，回提示符；`faultinfo` 可归因（L0 分级） |

## C. 自动化套件索引（`tests/`，115 个脚本）

> 每套件用独立 monitor 端口，禁止复用（`F2` 纪律）。以下是按主题的代表脚本，完整清单见 `tests/run_full_regress.sh`。

| 主题 | 脚本 |
|---|---|
| 格式/清单守卫 | `test_mkce.py`（CE01 格式 48 例）、`test_app_list.py`（apps.list 单一来源） |
| CE 回归（宿主模拟） | `ce_regress.py`、`ce_sim.py`（宿主模拟器）、`ro_builtin_regress.py` |
| 引导与冒烟 | `qemu_regress.py`、`smoke_merge.py`、`boot_time_probe.py`、`autostart_check.py` |
| 文件系统 | `disk_regress.py`、`fsrec_regress.py`、`lfn_regress.py`、`p2_mount_regress.py`、`recur_regress.py`、`fs_badname_regress.py` |
| 进程/作业 | `jobctl_regress.py`、`daemon_regress.py`、`pager_regress.py`、`spawn` 相关 `spawn_fail.ce` |
| 用户与权限 | `user_regress.py`、`caps_regress.py`、`pw_regress.py`、`group_regress.py`、`security_regress.py` |
| 内存/堆 | `heap_regress.py`、`vprobe_regress.py`、`pager_regress.py`、`v245_4gb_probe.py` |
| 图形 | `gfx24_regress.py`、`svga24_regress.py`、`videomode_regress.py`、`gfxtext_probe.py`、`gfxdesk_probe.py`、`aesthetic_probe.py`、`size_regress.py`、`cursor_v266_probe.py` |
| 鼠标/键位 | `mouse_regress.py`、`mouse_abi_v269_probe.py`、`gotoxy_v269_probe.py`、`keyprobe_v273_probe.py`、`probe_ps2.py` |
| 中文 | `ime_regress.py`、`cjk_width_regress.py`、`ttf_*`（宿主字形比对） |
| 音频 | `snd_regress.py`、`es1371_regress.py`、`sndinfo_v265_probe.py` |
| 网络 | `net_dhcp_probe.py`、`e1000_regress.py`、`tls_probe.py`、`d10_stream_probe.py`、`pkg_remote_v270_probe.py` |
| SSH | `ssh_v271_probe.py`、`ssh_v272_probe.py`、`ssh_step2_probe.py`、`ssh_hdiff_probe.py` |
| 桌面/包 | `desktop_pkg_v269_probe.py`、`p4_pkg_test.py`、`pkg_remote_v270_probe.py` |
| 诊断/崩溃 | `diag_regress.py`、`panic_regress.py`、`crash_attack.py`（对抗 20 项）、`isolation_verify.py`、`regression_after_fix.py` |
| 自举/语言 | `go_regress.py`、`test_cc.py`、`go_cases.py` |

## D. 判定陷阱与盲区

1. **FAIL 四要素**：命令、完整屏幕输出、`faultinfo` 输出、引导介质与数据盘来源（含 QEMU/VMware 版本）。新缺陷进 `CatOS-内核问题清单.md` 续号。
2. **gfx 下读屏无效**：`screen_text()` 读的是 TEXT buffer；必须 HMP `screendump` + PPM 放大核对。1× 截图看 8px 字形会误读成"缺字"。
3. **探针读不到中文**：`read_screen()` 只保留 ASCII 32–126；中文/IME/UTF-8 必须目视或截图判定，不要因中文断言 FAIL 判代码坏。
4. **QEMU `sendkey` 可能失效**：部分 Windows QEMU（>v10）同一 ISO 同序列不再产生按键（#34）；换 `-kernel` 或换版本，别先怀疑内核。
5. **鼠标无真实注入**：本 QEMU 无 PS/2 鼠标（`mouse` 的 pk 恒 0），鼠标 ABI 靠"CE==内核状态"断言；真实事件需 VMware/真机（F1）。
6. **text 模式无中文字形**：中文用例只在 gfx24/vbe24 跑；text 下中文丢弃属已知限制。
7. **端口/进程残留**：连续 boot 会因 monitor 端口复用误判；每次测试前清残留进程（F2）。
8. **版本拿错**：根目录有 v2.59–v2.78 多份 ISO，统一以 `catos-v2.78.iso` 为基线；VMware 的 VMX 指向哪份需先确认（G2）。
9. **编译≠完成**：任何改动必须过实机回归（`项目总纲.md` 第一部分 §6.1）。

---

## 附录：v2.59 原始用例（三篇原文，未改）

> 以下为 2026-11-23 合并时的原文，写作基线是 v2.59 / HEAD `46e5f47`。命令行为只增不减，故仍可作为旧功能回归依据；与 §A 冲突处以 §A 为准。
# 原文：CatOS-系统内测试指南.md

# CatOS 系统内测试指南

> **版本基线**：v2.59（HEAD `46e5f47`）；镜像 `catos-v2.59.iso`（md5 `a43ec5d2e969377cfccb51319bbb162c`）；源码唯一事实 `kernel_v25/`。
> **覆盖范围**：全部内置命令 + 全部 CE 应用 + 全部测试项。**不含文本模式（text）测试**——不安排 text 切换、text 下 IME/光标等用例；`text` 命令本身仍在命令总表中列出，因为它是客观存在的内置命令，但不对其安排测试项。
> **默认环境**：启动即 root 会话，图形模式 gfx24（800×600×24bpp，100×37 网格）；VBE 缺失时自动回退。
> **配套**：本指南只覆盖**引导后的系统内手工测试**。宿主侧自动化回归见 `tests/`，统一入口 `tests/run_full_regress.sh`（22 套件 / 270 断言 / 269 PASS）。

---

## 0. 环境、约定与判定

### 0.1 测试环境

| 项 | 方式 |
|---|---|
| VMware | 双击 `vmware/CatOS.vmx`（光驱 El Torito 引导，数据盘 IDE 0:0） |
| QEMU | `qemu-system-i386 -m 256 -cdrom catos-v2.59.iso -boot order=d -drive file=kernel_v25/catos_disk.img,format=raw,if=ide` |
| 网卡（可选） | `-netdev user,id=n0 -device pcnet,netdev=n0` |
| 登录 | 引导即 root 会话；`whoami` 应输出 `root` |

**禁止操作**：`format` 会重建 FAT32 卷并清空 `/bin`，只在专用测试盘上执行。

### 0.2 判定标准

- **PASS**：实际输出包含对应测试项所列要点；无 ring0 fault banner；提示符恢复。
- **FAIL**：输出与预期不符、出现 `page-fault` / triple fault / 死机，或命令无响应超过 10 秒。
- FAIL 记录四要素：命令、完整屏幕输出、`faultinfo` 输出、引导介质与数据盘来源（含 VMware/QEMU 版本）。新问题提交到 `docs/CatOS-内核问题清单.md`（现用至 #212，续号）。

### 0.3 命令调用通式

| 形式 | 行为 |
|---|---|
| `cmd [args]` | 裸命令名：先查当前目录，再查 `/bin`（PATH，默认 `/bin`），解析为 `.ce` |
| `run FILE.ce [args] [&]` | 按字面路径执行，**不做** PATH 查找；裸名只在 cwd 解析 |
| `cmd > F` / `cmd >> F` | 输出重定向到文件（ring0 内建命令经 tee，ring3 经 `out_file`） |
| `cmd | CMD2` | 管道，经 `kpipe_create()` 内建管道 |
| `cmd &` | 后台任务，`jobs` / `log` / `fg` / `bg` / `stop` 管理 |

---

## 1. 命令总表（全量）

来源：`kernel_v25/src/shell.c` 的 `cmd_help()` 与 `shell_exec()` dispatcher 合并去重。表格按功能分组，"测试项"列指向第 2 节编号；未列编号的命令按"最小验收点"验一次即可。

### 1.1 基础与信息

| 命令 | 别名 | 功能 | 最小验收点 | 测试项 |
|---|---|---|---|---|
| `help` | — | 列出全部命令，超屏走分页器 | 底部出现 `-- More ... --`，`q` 可退出 | 2.15 |
| `keys` | `shortcuts` | 按组列出全部绑定键位 | F1–F12、方向键、IME 键位齐全 | 2.17 |
| `clear` | `cls` | 清屏 | 屏幕清空，提示符回到顶行 | — |
| `lang [en\|zh]` | — | 显示/设置 UI 语言（`/etc/lang`） | 无参显示当前值；`zh` 后提示变中文 | — |
| `echo [-n] [-c COLOR] TEXT` | — | 回显文本 | `-n` 无换行；`-c` 上色 | — |
| `about` | — | 系统信息 banner | 版本 + `Bootloader: GRUB 2 (ISO9660 / El Torito)` | 2.1 |
| `uname` | — | OS 名与版本 | 输出 CatOS + 版本/架构 | 2.1 |
| `date` | — | RTC 日期时间 | 时间正确 | 2.1 |
| `mem` | — | 内存（multiboot）+ 堆统计 | Lower/Upper 数值出现 | 2.1 |
| `uptime` | — | 启动秒数 + 任务数 | 秒数递增 | — |
| `color NUM` | — | 前景色 0..255 | 即时生效 | — |
| `history` | — | 命令历史 | 与 Up/Down 调出的历史一致 | 2.17 |
| `autostart` | — | 重跑 `/etc/autostart` 脚本 | 脚本命令执行；不自动执行 `/etc/autoexec` | — |

### 1.2 文件系统

| 命令 | 别名 | 功能 | 最小验收点 | 测试项 |
|---|---|---|---|---|
| `dir [PATH]` | `ls` | 列目录 | 条目完整，超屏触发 More | 2.2, 2.15 |
| `cd [PATH]` | — | 切换 cwd（支持 `..` / `/`） | cwd 正确变化 | 2.2 |
| `mkdir PATH` | — | 建目录 | 目录出现 | 2.2 |
| `touch FILE` | — | 建空文件 | 文件出现 | 2.2 |
| `cat FILE` | — | 打印文件（≤64 KiB） | 内容逐字节一致 | 2.2 |
| `rm FILE` | — | 删文件 | 文件消失 | 2.2 |
| `rmdir DIR` | — | 删空目录 | 目录消失；非空应拒绝 | 2.2 |
| `cp SRC DST` | — | 复制文件 | 目标存在，权限归属正确 | 2.2 |
| `mv SRC DST` | — | 移动文件 | 源消失、目标出现 | 2.2 |
| `head [-n N] FILE` | — | 前 N 行（默认 10） | 行数正确 | — |
| `tail [-n N] FILE` | — | 后 N 行（默认 10） | 行数正确 | — |
| `wc [-l\|-w\|-c] FILE` | — | 行/词/字节计数 | 计数正确 | — |
| `tree [DIR]` | — | 目录树 | 层级与缩进正确 | — |
| `edit FILE` | — | 全屏编辑器 | `^O` 保存、`^X` 退出、`^K` 删行、`^U` 清空 | — |
| `pkg list\|info\|install\|remove NAME` | — | 本地包管理（`/pkg`） | 子命令按预期返回 | — |
| `format` | — | 重建 FAT32（y/n 确认） | 专用盘才测 | — |
| `fsck [--purge]` | — | FAT 镜像自检 + 目录树扫描 | `FAT #2 mirrors FAT #1` | 2.2 |
| `mount [1..4\|hd0pN] DIR` | — | 列出/绑定 FAT32 分区 | 绑定后目录可见 | — |
| `umount DIR` | — | 解绑分区 | 解绑后目录不可见 | — |
| `df` | — | 各挂载点用量 | 数值合理 | — |

### 1.3 进程与作业控制

| 命令 | 别名 | 功能 | 最小验收点 | 测试项 |
|---|---|---|---|---|
| `run FILE.ce [args] [&]` | — | 执行 CE 应用 | 输出正确 + `[task … exit(0)]` | 2.1, 2.4 |
| `ps` | — | 任务表（tid/uid/ring/state） | 新任务出现 | 2.4 |
| `kill TID` | — | 终止任务 | 目标消失；ring3 不得 kill ring0（`-2`） | 2.4, 2.3 |
| `sudo CMD` | — | 以 root 执行 | uid 提升 | 2.3 |
| `top` | — | 实时任务监控（任意键退出） | CPU% 滚动显示 | 2.4 |
| `jobs` | — | 后台作业 + 待读日志 | `&` 任务出现 | 2.4 |
| `log TID` / `log -f TID` | — | 显示/跟随后台作业输出 | 输出完整 | 2.4 |
| `fg [%]TID` / `bg [%]TID` / `stop [%]TID` | — | 前后台切换 / 挂起 | 状态按预期迁移 | 2.4 |
| `daemon CMD [args]` | — | 脱离为后台服务 | 父 shell 退出后仍存活 | — |
| `reboot` | — | 热重启 | 机器重启 | — |
| `hang` | — | 挂起 CPU（演示用） | 需重引导恢复 | 2.4 |

### 1.4 用户与权限

| 命令 | 别名 | 功能 | 最小验收点 | 测试项 |
|---|---|---|---|---|
| `whoami` | — | 当前用户 | `root` | 2.1, 2.3 |
| `id [USER]` | — | uid/gid/caps | uid=0（root） | 2.3 |
| `lsuser` | `users` | 列出账户 | 含 root 行 | 2.3 |
| `caps` | — | 能力位图例 + 会话 caps | 7 位图例可见 | 2.3 |
| `login U [PW]` | `su` | 切换会话用户 | 会话身份变化 | 2.3 |
| `useradd N [UID [GID [CAPS]]]` | — | 建用户（CAP_USERADM） | 出现在 `lsuser` | 2.3 |
| `userdel N` / `passwd N PW` / `chcaps HEX N` | — | 删用户/改口令/改 caps | 账户记录同步更新 | 2.3 |
| `groupadd G [MEMBER]` / `groups` | — | 建组 / 列组 | 成员关系正确 | 2.3 |
| `chmod OCT PATH` / `chown UID[:GID] PATH` | — | 持久化到 `/etc/acl` | 权限即时生效 | 2.3 |
| `useredit` | — | 用户编辑入口 | 交互可用 | — |

### 1.5 环境与硬件

| 命令 | 别名 | 功能 | 最小验收点 | 测试项 |
|---|---|---|---|---|
| `export K=V` | `setenv` | 设变量（空 V 删除） | `env` 可见 | — |
| `env` | — | 列出变量 | 含 `/etc/env` 持久化项 | — |
| `unset K` | — | 删除变量 | `env` 不再可见；`$K`/`${K}` 展开为空 | — |
| `alias` / `unalias` | — | 命令别名管理 | 别名生效/撤销 | — |
| `PATH` | — | 裸名查找路径（默认 `/bin`） | 修改后裸名解析变化 | — |
| `hwinfo` | — | 硬件全量（CPU/mem/disk/NIC/PCI） | 信息齐全不挂死 | — |
| `usb` | — | USB 状态 | 有设备时列出；无设备不报错 | — |

### 1.6 显示（图形后端）

| 命令 | 别名 | 功能 | 最小验收点 | 测试项 |
|---|---|---|---|---|
| `gfx` | — | Mode 13h，40×12，256 色 | 进入图形，提示符可用 | 2.5 |
| `vbe` | — | 800×600×32 VBE/SVGA | 真彩文本正常 | 2.5 |
| `gfx24` | `vbe24` | 800×600×24 绘图面 + 像素演示 | 红/绿/蓝/白块 + 渐变 + 8 色条 | 2.5 |
| `svgainfo` | — | VMware SVGA II PCI/FIFO 寄存器 | VMware 上 `updates>0` | 2.5 |
| `display` | — | 后端/网格/色深/CJK/字形数 | 与当前模式一致，字形数 6961 | 2.5, 2.12 |
| `vdiag` | — | VGA/VBE 寄存器诊断 | 输出不破坏既有屏幕内容 | 2.5 |
| `style SPEC` | — | 粗/斜/下划线/反色/删除/暗淡 | 即时生效，`off` 还原 | — |
| `rgb FG [BG]` | — | 0xRRGGBB 真彩色 | VBE 下 16.7M 色 | — |
| `palette I C` | — | 重定义 CLUT[I] | 颜色即时变化 | — |
| `size` | — | 网格/像素尺寸查询 | 与 `display` 一致 | — |
| `text` | — | 切回 80×25 文本（需物理 `Y` 确认） | **本指南不安排测试项** | — |

### 1.7 中文与输入法

| 命令/键 | 功能 | 最小验收点 | 测试项 |
|---|---|---|---|
| `F2` | 开关拼音 IME（shell 与 edit 通用） | 底部两行候选区出现/还原无残影 | 2.16 |
| `zhdemo` | 内建 UTF-8 中文 + 真彩演示 | 汉字与标点逐行正常 | 2.6 |
| `1..9` / `Space` / `-` `=` `[` `]` / `←` `→` / `Esc` / `Enter` | 选字/首选/翻页/取消/原样上屏 | 见 2.16 | 2.16 |

### 1.8 音频（SB16）

| 命令 | 功能 | 最小验收点 | 测试项 |
|---|---|---|---|
| `sndinfo` | SB16 探测、DSP 版本、IRQ/DMA | 型号/端口/IRQ/DMA 可见 | 2.7 |
| `sndtest [HZ MS]` | 播放音调（默认 440 Hz / 800 ms） | `SNDTEST PASS` | 2.7 |

### 1.9 网络（PCnet）

| 命令 | 功能 | 最小验收点 | 测试项 |
|---|---|---|---|
| `curl URL` | HTTP GET | 返回页面内容 | 2.10 |
| `ping IP` | ICMP echo（3 次） | 3 次回显 | 2.10 |
| `dns NAME` | 解析主机名 | 返回 IP | 2.10 |
| `udp IP PORT TEXT` | 发一个 UDP 数据报并等回复 | 收到回包或超时明确 | 2.10 |
| `netdiag` | NIC/驱动状态 | PCnet 或 e1000 状态可见 | 2.10 |
| `netcfg` | 网络配置 | 无 NIC 时明确 NOT FOUND | 2.10 |

### 1.10 诊断与故障

| 命令 | 别名 | 功能 | 最小验收点 | 测试项 |
|---|---|---|---|---|
| `faultinfo` | — | 最近一次致命故障（向量/CR2/PF bits） | 崩溃后有完整记录 | 2.4 |
| `crashlog [--full]` | — | 保存的 panic 记录 | 记录存在，`--full` 输出原始 | 2.4 |
| `bsod [--no-reboot]` | `blue` | 全屏崩溃报告 | 报告完整可切换 | — |
| `panicmode [reboot\|halt]` | — | 崩溃后动作（`/etc/panicmode`） | 读值/设值正确 | — |

### 1.11 趣味命令

| 命令 | 功能 | 最小验收点 |
|---|---|---|
| `cowsay [-f tux] MSG` | ASCII 牛/Tux 说话 | 图形正常 |
| `sl` | 蒸汽火车驶过 | 动画不挂死 |
| `rain` | 全屏数字雨（tui-preview） | 有绿色雨幕，任意键立即退出，退出后背景/前景恢复 | 
| 中文标点（v2.77） | IME 下标点 | F2 后敲 `. , ? !`，得到 `。 ， ？ ！` 各两格，无 `??` | 
| IME 候选框（v2.77） | 候选框不随滚动 | F2 输 `ni` 出候选框，连按 PgUp/PgDn 两页，框仍钉在底部 | 
| `rm -r` + `fsck`（v2.77） | 递归删除一致性 | 3 层树 + 文件删除后 `fsck` 报 FAT mirror OK / directory tree OK | 
| `pkg install`（v2.77） | 签名强校验 | `pkg install bc` 成功；旧摘要包被 `SIGNATURE VERIFICATION FAILED` 拒绝 | 
| `curl https://`（v2.78） | TLS 1.2 客户端 | 宿主机 `openssl s_server -tls1_2 -cipher ECDHE-RSA-AES128-SHA256 -accept 8443`；guest `netcfg 10.0.2.15 10.0.2.2` 后 `curl https://10.0.2.2:8443/`，屏幕出现固定串 `CATOS_TLS_OK_278` |
| `pkg fetch` 大包（v2.78） | 流式下载/落盘（>16 KiB） | 46 KiB 签名包经 Content-Length 与 chunked 两路：屏幕 `got 46456 bytes` → `installed …` → 运行打印 `D10 BIGPKG OK` + `46 KiB payload resident` |
| 大 CE 镜像（v2.78） | 帧分配器保留用户虚存区 | 装 46 KiB 的 CE 应用后可直接运行，不出现 exit(0) 零输出；回归：小镜像应用行为不变 |
| `shutdown`（v2.77） | ACPI 关机 | VMware 真机：`shutdown` 应直接断电，不再回退重启（云端无法验证） | 
| `help`（v2.76） | 长输出按键跳过 | 输出超过一屏后按任意键，立即停止剩余输出并回提示符，按下的键保留在输入行 | 
| `help > /tmp/h.txt`（v2.76） | 重定向不被跳过 | 重定向时按键不截断文件，`wc -l /tmp/h.txt` 为完整行数（简体中文 220 行） | 
| 滚动历史（v2.76） | 1 万行回溯 | 连跑 3 次 `help` 后 PgUp 多页仍可看到最早输出；PgDn 回到底部画面与滚动前一致 | 
| `train` | ASCII 蒸汽火车横穿（tui-preview） | 两趟后自动结束，任意键可提前退出，无残影 |
| `fortune` | 随机箴言 | 有输出 |
| `cal [YEAR]` | 月历/年历 | 日期正确 |
| `yes [TEXT]` | 重复输出直到 Ctrl+C | Ctrl+C 可中断 |

---

## 2. 测试项（全量）

### 2.1 冒烟测试（引导后 30 秒内，必过）

| # | 命令 | 预期要点 | 失败含义 |
|---|---|---|---|
| S1 | （引导完成） | ASCII logo + `root@catos:/$` 提示符 | 引导链/显示故障 |
| S2 | `about` | 版本 + `Bootloader: GRUB 2` + `Runs on: QEMU / VMware / Bochs / real PC` | 内核镜像陈旧 |
| S3 | `uname` | CatOS + 版本/架构 | — |
| S4 | `ls /bin` | ≥24 个 `.ce`（hello/calc/sndtest/vprobe/punctest…） | 数据盘未挂载或应用缺失 |
| S5 | `mem` | Lower/Upper 内存数值 + 堆统计 | 内存探测异常 |
| S6 | `run /bin/hello.ce` | hello 输出 + `[task … exit(0)]` | CE 加载器故障 |
| S7 | `date` | 正确日期时间 | RTC 故障 |
| S8 | `cat /etc/passwd` | root 行可见 | 用户库/权限异常 |

### 2.2 文件系统

| 命令序列 | 预期 |
|---|---|
| `mkdir /tmp/t1` → `touch /tmp/t1/a.txt` → `ls /tmp/t1` | 目录与文件均出现 |
| `cp /bin/hello.ce /tmp/h2.ce` → `ls /tmp/h2.ce` | 存在，0644 归属当前用户 |
| `mv /tmp/h2.ce /tmp/h3.ce` → `ls /tmp` | h2 消失、h3 出现 |
| `cat /tmp/h3.ce` | 二进制乱码可读出 |
| `rm /tmp/h3.ce` → `ls /tmp` | h3 消失 |
| `fsck` | `FAT #2 mirrors FAT #1` |
| `cd /bin` → `ls` → `cd /` | cwd 切换正常 |

### 2.3 用户与权限

| 命令序列 | 预期 |
|---|---|
| `whoami` / `id` | `root` / uid=0 |
| `useradd alice` → `lsuser` / `users` | alice 出现在列表 |
| `passwd alice` → `login alice`（输入两次口令） | 会话切到 alice |
| `run /bin/hello.ce` | 正常（`/bin` 为 0755） |
| `cp /bin/captest.ce /tmp/nox.ce` → `run /tmp/nox.ce` | **err -2**（无 execute bit） |
| `chmod +x /tmp/nox.ce`（或 755）→ `run /tmp/nox.ce` | 正常执行 |
| `chmod 000 /tmp/nox.ce` → `run /tmp/nox.ce` | **err -13**（EACCES） |
| `run /TMP/NOX.CE`、`run /tmp/../tmp/nox.ce`、`run /tmp//nox.ce` | 全部 **err -13**（#97 回归） |
| `su root`（root 无口令） | 切回 root |
| `run /bin/captest.ce` | root：`net_init=-1`（硬件缺失）；零能力用户：`-13`（权限拒绝） |
| `run /bin/perms.ce` | 权限探针输出，无 -13 意外项 |
| ring3 `kill` ring0 任务 | 返回 `-2`（EPERM） |

注：root 仅在文件无 execute bit 或 ACL 明确 DENY 时被拒绝。

### 2.4 进程与故障诊断

| 命令序列 | 预期 |
|---|---|
| `run /bin/spinner.ce` → `ps` | 任务表出现 spinner（多任务并发） |
| `top` | 任务/负载滚动，任意键退出 |
| `run /bin/hello.ce &` → `jobs` → `log <tid>` | 后台任务出现，日志可读 |
| `fg <tid>` / `stop <tid>` / `bg <tid>` | 状态按预期迁移 |
| `kill <tid>` | 目标任务结束 |
| `run /bin/crash.ce` → `faultinfo` | `page-fault ring0x… task crash tid… eip=… cr2=… err=… efl=… at tick…` + PF bits |
| 故障后 `ps` / `ls` | 系统存活，提示符恢复（隔离性） |
| `crashlog` / `crashlog --full` | 有 panic 记录，完整可读 |
| `hang` | 挂起当前会话（重引导恢复） |

### 2.5 图形显示

| 命令 | 预期 |
|---|---|
| `gfx` | Mode 13h 40×12 256 色，提示符可用 |
| `vbe` | 模式列表与真彩文本终端（QEMU std / QEMU vmware / VMware 均可用） |
| `gfx24` / `vbe24` | 800×600 真彩诊断图：红/绿/蓝/白块 + 256 级渐变 + 8 色条 |
| `svgainfo` | VMware：`SVGA II adapter : present`、`id : 0x90000002`、FB/FIFO BAR、`updates>0`；QEMU std 提示无该设备 |
| `display` | `graphics HW : VMware SVGA II (15AD:0405)` 或 Bochs；`last gfx24 hw` / `hw_bpp` / `stride`；字形数 6961 |
| `vdiag` | 不破坏之前 `ls` 的显示内容；`glyphA` 十六进制一行内联完整 |
| `palette 4` / `rgb` / `style` / `color` | 颜色/属性即时生效 |
| 图形模式光标 | 空格 → 1 px 下划线；有字符 → 背景反色；0.5 s 闪烁；换行/编辑后不留残光标 |

**VMware 显卡专项**：依次 `svgainfo` → `vbe` → `vbe24` → `gfx24`。四条都应进入 800×600 真彩且非黑屏；`updates` 必须 > 0。若 `vbe24` 报 unknown command 或 `gfx24` 报 no VBE/Bochs-dispi，说明镜像早于 v2.20；若 `updates` 恒为 0，说明早于 v2.21。

### 2.6 中文与 CJK 字形

| 命令 | 预期 |
|---|---|
| `run /bin/punctest.ce` | 全角标点表；首行含 **`glyphs=6961`**；`、。！？` 渲染为全角宽字形，无 `?` |
| `run /bin/zhdemo.ce` | 中文段落逐行显示，汉字+标点正常 |

**等宽逐像素断言**：CJK 步进必须是 ASCII 的 2 倍。用 `vbe` 欢迎行下划线当尺子——纯 ASCII 行 49 格 ⇒ **392 px**（8 px/格）；混排行 12 ASCII + 14 CJK ⇒ **320 px** ⇒ CJK 步进 = 16 px = 2×ASCII。退化成 1 格会得到 224 px、3 格 416 px，均判失败。

### 2.7 音频

| 命令 | 预期 |
|---|---|
| `sndinfo` | 声卡型号/端口/IRQ/DMA；ES1370/1371 可见时另报状态 |
| `sndtest 440 800` | `snd: playing 440 Hz for 800 ms` + `snd: irqs=1 busy=0` + **`SNDTEST PASS`** |
| `run /bin/sndtest.ce` | **`SND CE PASS`**（ring3 路径） |
| 无声卡配置 | 静音降级，**不报错不挂死** |

### 2.8 内存与堆

| 命令 | 预期 |
|---|---|
| `run /bin/heaptest.ce` | 堆分配/释放全 PASS |
| `run /bin/allocprobe.ce` | 探针全 found |
| `run /bin/mmaptest.ce` | mmap/munmap 往返成功 |
| `run /bin/vprobe.ce fr` | **`VPROBE fr PASS`**（跨页校验，#78）；wr/fw/pw 用例的 `VPROBE-WR ABCDE` 等字符串正确回显 |

### 2.9 自举工具链

| 命令 | 预期 |
|---|---|
| `run /bin/cc.ce <示例>` | 阶段 1–3 编译器正常产出 |
| `run /bin/go.ce` | REPL/测试通过（语法错误报行号） |
| `run /bin/ocntools.ce` | 工具集菜单（文件管理器/十六进制编辑等） |

### 2.10 网络

| 命令 | 预期 |
|---|---|
| `netcfg` / `netdiag` | 无 NIC 时提示 NOT FOUND / 初始化失败 |
| `run /bin/captest.ce` | root `net_init=-1`（硬件缺失）；零能力用户 `-13`（权限拒绝），两者语义不同 |
| `ping` / `curl` / `dns` / `udp` | 需 QEMU 带 `-device pcnet` 才可用；VMware vmxnet3/e1000 不受支持，属已知限制 |

### 2.11 游戏（综合体验）

| 命令 | 预期 |
|---|---|
| `run /bin/adofai.ce` | 菜单出现；有节奏音效（SB16）；pollkey 按键响应；ESC 退出干净 |
| `run /bin/demo.ce` / `run /bin/gfxdemo.ce` | 图形演示无花屏 |

### 2.12 视频模式持久化

| # | 输入 / 操作 | 预期 |
|---|---|---|
| V1 | 冷启动，不输入模式命令 | 默认进入 gfx24（800×600×24bpp，100×37 网格）；`display` 后端含 vbe24，CJK=yes |
| V3 | `/etc/videomode` 写 `gfx`，重启 | 进入 Mode 13h，40×12，256 色 |
| V4 | 写 `vbe`，重启 | 进入 32bpp VBE，100×37 |
| V5 | 写 `vbe24`，重启 | 进入 24bpp vbe24，100×37 |
| V6 | 写非法值（如 `foo`）或删除文件后重启 | 不卡启动；回退默认 gfx24；提示符可用 |
| V7 | 无 VBE 环境请求 vbe / vbe24 | `enter()` 失败后还原进入前的可用后端，不黑屏、不 triple fault |
| V8 | 各模式下执行 `display` | 后端名、网格、像素、色深、CJK、字形数与当前模式一致（6961） |

> 本指南不设 text 取值（`videomode=text`）的测试项。

### 2.13 鼠标与剪贴板（gfx24/vbe24 下）

| # | 操作 | 预期 |
|---|---|---|
| MO1 | 移动鼠标 | 可见光标随鼠标移动，方向一致 |
| MO2 | 滚轮上一格/下一格 | 回溯视图或正文滚动 3 行/notch；到顶/到底不越界抖动。注：QEMU 6.2 板载 PS/2 鼠标 HMP 不转发 dz，guest 端覆盖为 deferred |
| MO3 | 左键按下拖动一段文本后松开 | 途经文本高亮选中；松开后选中态保持 |
| MO4 | 先拖选，再在输入行按中键 | 文本粘贴到光标处；UTF-8 多字节完整、不截断半个汉字 |
| MO5 | 选中状态下按右键 | 取消选中，不向输入行写入 |
| MO6 | `clip`（无参） | 显示剪贴板内容（空则提示空） |
| MO7 | `clip TEXT` 写入后中键粘贴 | 逐字节一致；中文三字节 UTF-8 完整 |
| MO8 | IME 候选区打开期间操作鼠标 | 鼠标选择与 shell/编辑器文本操作互不破坏（候选区点选属规划阶段三） |

选择边界：可选区止于 `rows-3`，底部两行预留给 IME 候选区。

### 2.14 回溯滚动（Ctrl 组合键）

| # | 操作 | 预期 |
|---|---|---|
| SC1 | 输出超过一屏后按 Ctrl+↑ | 上滚 1 行/次，连续按逐行上移 |
| SC2 | 回溯状态按 Ctrl+↓ | 下滚 1 行/次，回到实时视图后停止 |
| SC3 | Ctrl+PgUp | 上翻一页，页高 = `con_rows()`（可视行数） |
| SC4 | Ctrl+PgDn | 下翻一页，页高 = `con_rows()`；回实时视图后停止 |
| SC5 | 分页器 More 期间按 Ctrl+PgUp/PgDn | 不丢键、不死锁；优先级按最终实现如实记录 |

实现为增量单行搬移：一次按键 = 整屏行位移一行 + 仅重绘新补入的一行；到顶/到底不重复行、不空行；任意非回溯键返回活屏。

### 2.15 通用分页器（v2.59）

| # | 操作 | 预期 |
|---|---|---|
| PG1 | `help`（超一屏） | 底部 `-- More (Enter next line, Space page, q quit) --`；正文按 `con_rows()-2` 行缓冲，不占底部两行 |
| PG2 | More 状态按 Enter | 推进一行；提示仍在 |
| PG3 | More 状态按 Space | 推进一屏（页高 = 可视行缓冲行数） |
| PG4 | More 状态按 q | 退出分页；剩余输出不再等待，提示符立即可用 |
| PG5 | `ls /bin`（≥24 项） | 超屏触发 More；Enter/Space 可翻完全部条目，不丢条目 |
| PG6 | `ls /etc` | 列出当前条目；不足一屏不出现 More |
| PG7 | `help > /tmp/f` | 不暂停，一次性写完；`cat /tmp/f` 与完整 help 一致 |
| PG8 | `help &` | 不阻塞提示符；分页器在后台关闭；输出可经 `log` 查看 |
| PG9 | `cat` 长文件（≥两屏） | 触发同一 More 流程，三键行为同 PG2–PG4 |
| PG10 | 分页期间 Ctrl+PgUp/PgDn | 与分页器互不抢占死锁，行为如实记录 |

### 2.16 F2 中文输入法（图形模式下，gfx24/vbe24）

| # | 操作 | 预期 |
|---|---|---|
| I1 | shell 提示符按 F2 | IME 开启，底部两行候选区出现；再按 F2 关闭，区域还原无残影 |
| I2 | 输入拼音如 `zhong` | 拼音串内联显示在输入行光标处，**不进候选区**；退格可删字母 |
| I3 | 观察候选区两行布局 | 倒数第二行整行横线铺满可视列；最后一行 `1.好 2.号 3.浩` 格式 |
| I4 | 宽列（vbe24 100 列）与窄列（gfx 40 列）分别触发 | 每页候选数按列宽自适应，保底 9 个；不放半个汉字；窄列先省略候选间空格 |
| I5 | 数字 `1..9` 选字；Space 首选；`-`/`=`/`[`/`]` 与 `←`/`→` 翻页；Esc 取消；Enter 原样上屏 | 行为与键位表一致；Esc 清空拼音串并还原候选区 |
| I6 | gfx24（vbe24）模式下重复 I1–I5 | 横线行、候选行、汉字绘制正确，无错位/截断 |
| I7 | 候选上屏后检查输入行/编辑器 | 汉字插入光标处，三字节 UTF-8 完整；`^S` 保存后 `cat` 回显一致 |
| I8 | 候选多于一页时翻页 | 显示当前页/总页数；越界翻页回到第 1 页；空槽位不提交脏字符 |
| I9 | `edit` 编辑器内按 F2 输入中文 | 开关与上屏正常；退出编辑器时 IME 强制关闭 |

> 数据边界：离线 `pinyin.inc` 共 400 音节 / 6,763 汉字；单音节候选最多 104 个（`ji`）。阶段一不支持词组/联想、模糊音、自造词、每会话状态隔离。口令掩码输入不经过 IME。

### 2.17 键位与命令补全

| # | 操作 | 预期 |
|---|---|---|
| K1 | `keys` | 输出分组图例：编辑/滚动/历史/输入法/控制台/文件，与 `docs/设计手册.md` 第四部分一一对应 |
| K2 | 对照实际按键抽查 | 图例所列键位与 `kb_getkey()` 返回码一致；无乱码、无缺项、无重复定义 |
| K3 | `did you mean:` 提示后按物理 `y` 一次 | 执行建议命令；不重复执行 |
| K4 | Tab / Up / Down / Home / End / Ctrl+Left/Right | 补全与行编辑行为符合 `docs/设计手册.md` 第四部分 |

---

## 3. CE 应用清单（`/bin/*.ce`，54 个）

裸名可直接执行（先 cwd 后 `/bin`）；`run` 需带路径。内置副本仅作 fallback，预装盘为权威来源。

| 类别 | 应用 |
|---|---|
| 演示与工具 | `hello` `calc` `bc` `cc` `go` `curl` `netinfo` `hw` `sysinfo` `ocntools` `cfm` `cmenu` `cgreet` `gen_ce` `rps` `adofai` |
| 图形与中文 | `demo` `gfxdemo` `gfx24` `punctest` `zhdemo` `charness` `ctheme` |
| 诊断与探针 | `allocprobe` `captest` `crash` `heaptest` `memlog` `mmaptest` `perms` `sndtest` `vprobe` `stats` `dltest` `minimal_ce` `sink_ce` `test_ab` `ctop` `spinner` |
| 对抗/边界用例（atk_*） | `atk_args` `atk_autostart` `atk_fs` `atk_gfx` `atk_heap` `atk_len` `atk_mmap` `atk_net` `atk_pipe` `atk_proc` `atk_stack` `atk_sysnr` `atk_sysprot` `atk_taskstat` `atk_wait` |

> `zhdemo` 既在 help 表列出，也以 `/bin/zhdemo.ce` 存在；裸名走 PATH 解析。

---

## 4. 已知非缺陷项（勿报 bug）

| 现象 | 说明 |
|---|---|
| `PCnet NOT found` / `net_init=-1` | 未带网卡参数或硬件缺失 |
| VMware vmxnet3/e1000 网络不可用 | 当前驱动不支持，已知限制 |
| `format` 后 `/bin` 清空 | 内核用内嵌应用兜底重建；只有预装盘才会满 |
| 引导早期 GRUB `file not found` | 用了未打补丁的 `build/catos_work.iso` 模板；请用 `catos-v2.59.iso` |
| 磁盘镜像引导卡死 | 数据盘 MBR 无引导代码，本来就不是引导介质 |
| IME 候选区中文在部分后端缺字形 | 见问题清单 #117，不报错 |

---

## 5. 自动化回归对照

系统内手工测试通过后，宿主侧跑全套回归（各自独立 monitor 端口）：

```
宿主: test_mkce 48  test_cc 10  test_app_list 24 apps  ce_regress 11P/17S/0F
实机: qemu_regress 12/12  user_regress 15/15  ot_regress ALL  diag_regress 4/4
      disk_regress 5/5  heap_regress 4/4  go_regress 7/7  vprobe_regress 10/10
      snd_regress 6/6  gfx24_regress 0 failed  security_regress 17/17
      e1000_regress 0 failed  es1371_regress 0 failed
实机(-vga vmware): svga24_regress 34/34 PASS（端口 4467）
实机(-vga std):    cjk_width_regress 0 failed（端口 4493）
统一入口:          tests/run_full_regress.sh（22 套件 / 270 断言 / 269 PASS）
```

| 手工测试项 | 对应回归脚本 |
|---|---|
| 2.3 权限/大小写变体 | `tests/security_regress.py`（17 用例，4476） |
| 2.4 faultinfo/隔离 | `tests/diag_regress.py`（4463） |
| 2.6 punctest | `tests/security_regress.py`（glyph-count-6961） |
| 2.6 CJK=2×ASCII 等宽 | `tests/cjk_width_regress.py`（4493，逐像素） |
| 2.7 音频 | `tests/snd_regress.py`（4460） |
| 2.5 24bpp 逐像素（Bochs） | `tests/gfx24_regress.py`（4462） |
| 2.5 VMware SVGA II 专项 | `tests/svga24_regress.py`（4467，`-vga vmware`） |
| 2.8 vprobe | `tests/vprobe_regress.py`（4461） |
| 2.2 文件系统 | `tests/disk_regress.py` |
| 2.3 用户全套 | `tests/user_regress.py` |
| 2.12 视频模式 | `tests/videomode_regress.py` |
| 2.13 鼠标/剪贴板 | `tests/mouse_regress.py` |
| 2.14 回溯 | `tests/mouse_regress.py`（回溯用例） |
| 2.15 分页器 | `tests/pager_regress.py` |
| 2.16 IME 候选区 | `tests/ime_regress.py` |

---

## 6. 记录表模板

```
日期:              引导方式: VMware / QEMU     镜像: catos-v2.59.iso md5=__________
冒烟 S1~S8:        [ ] 全 PASS    失败项: ______
命令总表抽查:      1.1[_] 1.2[_] 1.3[_] 1.4[_] 1.5[_] 1.6[_] 1.7[_] 1.8[_] 1.9[_] 1.10[_] 1.11[_]
测试项:            2.1[_] 2.2[_] 2.3[_] 2.4[_] 2.5[_] 2.6[_] 2.7[_] 2.8[_] 2.9[_]
                   2.10[_] 2.11[_] 2.12[_] 2.13[_] 2.14[_] 2.15[_] 2.16[_] 2.17[_]
自动化回归:        run_full_regress.sh [ ] ____/270
FAIL 明细:         （命令 / 预期 / 实际现象 / faultinfo）
结论:              通过 / 带缺陷通过 / 不通过     测试人: ______
```

---

# 原文：CatOS-系统内测试文档.md

# CatOS 系统内测试文档

## 本轮章节（2026-11-18，HEAD `aae8640`）

> 本轮新增功能（视频模式持久化、鼠标/剪贴板、回溯翻页、v2.59 分页器、F2 两行候选区、`keys` 命令）的手工/脚本验收步骤与“输入/预期/实测结果”三列记录表，见独立文件：
>
> **系统内测试文档-2026-11-18.md（本文件第 3 篇原文，正文紧随本条）**
>
> 自动化对照：`tests/videomode_regress.py`、`tests/mouse_regress.py`、`tests/pager_regress.py`（并行子代理实现中）；IME 候选区对应 `docs/设计手册.md` 第三部分 阶段一。下方为历史版本测试手册，口径沿用。

---

> 适用版本：v2.20（CE ABI 66 槽，含 VMware SVGA II 图形后端；`3ed5cad` v2.19 及以后）
> 测试介质：`kernel_v25/catos.iso`（GRUB isohybrid，光驱或硬盘引导均可）
> 数据盘：`kernel_v25/catos_disk.img`（FAT32，预装 24 个 CE 应用）
> 配套自动化回归：`tests/`（本手册只覆盖**引导后的系统内手动测试**）

---

## 1. 测试环境准备

| 项 | 方式 |
|---|---|
| VMware | 双击 `vmware/CatOS.vmx`（光驱 El Torito 引导，数据盘 IDE 0:0） |
| QEMU | `C:\qemu-portable\qemu-system-i386.exe -m 256 -cdrom kernel_v25/catos.iso -drive file=kernel_v25/catos_disk.img,format=raw,if=ide -boot order=d` |
| 登录 | 引导即 root 会话（`whoami` 应输出 `root`） |

**禁止操作**：`format` 会重建 FAT32 卷并清空 `/bin`。只有在专用测试盘上才允许执行。

---

## 2. 冒烟测试（约 10 分钟，必过）

引导完成后 30 秒内依次执行，全部符合预期即可判定本次引导健康：

| # | 命令 | 预期输出要点 | 失败含义 |
|---|---|---|---|
| S1 | （引导完成） | ASCII logo + `root@catos:/$` 提示符 | 引导链/显示故障 |
| S2 | `about` | 版本号、`Bootloader: GRUB 2 (ISO9660 / El Torito)`、`Runs on: QEMU / VMware / Bochs / real PC` | 内核镜像陈旧（对照问题清单 #99 检查构建链） |
| S3 | `uname` | CatOS + 版本/架构 | — |
| S4 | `ls /bin` | ≥24 个 `.ce`（hello/calc/sndtest/vprobe/punctest/…） | 数据盘未挂载或应用缺失 |
| S5 | `mem` | Lower/Upper 内存数值 + 堆统计 | 内存探测异常 |
| S6 | `run /bin/hello.ce` | hello 输出 + `[task … exit(0)]` | CE 加载器故障 |
| S7 | `date` | 正确的日期时间 | RTC 故障 |
| S8 | `cat /etc/passwd` | root 行可见（root:0:…） | 用户库/权限异常 |

---

## 3. 分模块测试项

### 3.1 文件系统

| 命令序列 | 预期 |
|---|---|
| `mkdir /tmp/t1` → `touch /tmp/t1/a.txt` → `ls /tmp/t1` | 目录与文件均出现 |
| `echo` 无写文件能力，写入用 CE：`run /bin/hello.elf` 不应影响 FS | — |
| `cp /bin/hello.ce /tmp/h2.ce` → `ls /tmp/h2.ce` | 存在，0644 归属当前用户 |
| `mv /tmp/h2.ce /tmp/h3.ce` → `ls /tmp` | h2 消失、h3 出现 |
| `cat /tmp/h3.ce` | 二进制乱码可读出（cat 不做权限外的过滤） |
| `rm /tmp/h3.ce` → `ls /tmp` | h3 消失 |
| `fsck` | `FAT #2 mirrors FAT #1`（镜像 FAT 一致性） |
| `cd /bin` → `ls` → `cd /` | cwd 切换正常 |

### 3.2 用户与权限（v2.15 行为）

| 命令序列 | 预期 |
|---|---|
| `whoami` / `id` | `root` / uid=0 |
| `useradd alice` → `lsuser` / `users` | alice 出现在列表 |
| `passwd alice` → `login alice`（输入两次口令） | 会话切到 alice |
| `run /bin/hello.ce` | 正常（`/bin` 为 0755） |
| `cp /bin/captest.ce /tmp/nox.ce` → `run /tmp/nox.ce` | **err -2**（无 execute bit，PERM_EXEC 拒绝） |
| `chmod +x /tmp/nox.ce`（或 `chmod 755`）→ `run /tmp/nox.ce` | 正常执行 |
| `chmod 000 /tmp/nox.ce` → `run /tmp/nox.ce` | **err -13**（EACCES） |
| 大小写/路径变体（#97 回归） | `run /TMP/NOX.CE`、`run /tmp/../tmp/nox.ce`、`run /tmp//nox.ce` 全部 **err -13** |
| `su root`（root 无口令） | 切回 root |
| `run /bin/captest.ce` | 打印各 syscall 原始返回码（root：net_init=-1 是硬件缺失非拒绝） |
| `run /bin/perms.ce` | 权限探针输出，无 -13 意外项 |

root 拒绝例外：root 仅在文件无 execute bit 时被 -2 拒绝；ACL 明确 DENY 时也拒绝。

### 3.3 进程与故障诊断

| 命令序列 | 预期 |
|---|---|
| `run /bin/spinner.ce` → 另用 `ps` | 任务表出现 spinner（多任务并发） |
| `top` | 任务/负载滚动显示，Ctrl+C 可退出 |
| `kill <tid>` | 目标任务结束 |
| `run /bin/crash.ce` → 故障后 `faultinfo` | `page-fault ring0x… task crash tid… eip=… cr2=… err=… efl=… at tick…` + PF bits（present/write/user） |
| 故障后系统应存活 | 提示符恢复，`ps`/`ls` 正常（隔离性） |
| `hang` | 挂起当前 shell 会话（演示用，重引导恢复） |

### 3.4 显示子系统

| 命令 | 预期 |
|---|---|
| `text` | 回到 80x25 文本模式（退出图形需按 `Y` 确认） |
| `gfx` / `vbe` | 模式列表与真彩文本终端（QEMU std / QEMU vmware / VMware 均可用） |
| `gfx24` / `vbe24` | 切到 800x600 并绘制诊断图（红/绿/蓝/白块 + 256 级渐变 + 8 色条）；`vbe24` 是 v2.20 新增别名 |
| `svgainfo` | VMware 上显示 `SVGA II adapter : present`、`id : 0x90000002`、FB/FIFO BAR、`updates>0`（图形模式退出后仍可查）；QEMU std 上提示无该设备 |
| `display` | `graphics HW : VMware SVGA II (15AD:0405)` 或 Bochs；`last gfx24 hw` / `hw_bpp` / `stride` |
| `gfx24` 后 `text` | 无残影回到文本（#57/#100 修复验证点） |
| `vdiag` | 诊断输出 **不破坏**之前 `ls` 的显示内容（#100）；`glyphA` 十六进制一行内联完整（#100） |
| `palette 4` / `rgb` / `style` / `color` | 文本颜色/属性即时生效，恢复用 `text` |
| 图形模式光标 | 当前格**无字符**⇒ 格子末行 1 px **下划线**；当前格**有字符**⇒ 该格 **背景反色**；光标**闪烁**（0.5 s 半周期）；换行/编辑后**不留残光标**（#109/#110） |

**VMware 显卡专项（v2.20）**：在 VMware Workstation 里依次执行
`svgainfo` → `vbe` → `text` + Y → `vbe24` → `text` + Y → `gfx24`。
四条都应进入 800x600 真彩且显示完整（不是黑屏）；
`svgainfo` 的 `updates` 必须大于 0（FIFO 刷新在工作）。
若 `vbe24` 报 unknown command 或 `gfx24` 报 no VBE/Bochs-dispi，说明
镜像仍是 v2.19 或更早。

> **v2.21 补充（#107/#108）**：VMware 上 `vbe` 黑屏的根因是显存映射**可缓存**
> （设备读不到 CPU 的写回缓存）与 **IRQ0 从不调用 `svga_tick()`**（FIFO 拿不到
> `UPDATE` 命令）。两者都已修：`paging.c` 的 LFB 映射加了 PCD 位，
> `sched_timer_tick()` 直接驱动 `svga_tick()` + `con_tick()`。
> 若 `svgainfo` 的 `updates` 恒为 0，说明镜像早于 v2.21。

### 3.5 中文与 CJK 字形（#101）

| 命令 | 预期 |
|---|---|
| `run /bin/punctest.ce` | 打印全角标点表；首行含 **`glyphs=6961`**；`、。！？`等渲染为**全角宽字形**，无 `?` |
| `run /bin/zhdemo.ce` | 中文段落逐行显示，汉字+标点均正常 |

**等宽逐像素断言**（v2.21 新增 `tests/cjk_width_regress.py`，monitor 4493）：
CJK 字形步进必须是 ASCII 的 **2 倍**。用 `vbe` 欢迎行的下划线连续段当尺子——
纯 ASCII 行 49 格 ⇒ **392 px**（8 px/格）；混排行 12 ASCII + 14 CJK ⇒
**320 px** ⇒ CJK 步进 = (320−12×8)/14 = **16 px = 2×ASCII**。
若退化成 1 格宽该行会是 224 px、3 格宽 416 px，都会被判失败。

### 3.6 音频（SB16）

| 命令 | 预期 |
|---|---|
| `sndinfo` | 声卡型号/端口/IRQ/DMA 信息 |
| `sndtest 440 800` | `snd: playing 440 Hz for 800 ms` + `snd: irqs=1 busy=0` + **`SNDTEST PASS`** |
| `run /bin/sndtest.ce` | **`SND CE PASS`**（ring3 路径） |
| VMware/QEMU 无声卡配置时 | 静音降级、**不报错不挂死** |

### 3.7 内存与堆

| 命令 | 预期 |
|---|---|
| `run /bin/heaptest.ce` | 堆分配/释放全 PASS |
| `run /bin/allocprobe.ce` | 探针全 found |
| `run /bin/mmaptest.ce` | mmap/munmap 往返成功 |
| `run /bin/vprobe.ce fr` | **`VPROBE fr PASS`**（用户缓冲跨页校验，#78 回归探针）；其余用例 wr/fw/pw：`VPROBE-WR ABCDE` 等字符串正确回显 |

### 3.8 自举工具链

| 命令 | 预期 |
|---|---|
| `run /bin/cc.ce <示例>` | 阶段 1-3 编译器正常产出 |
| `run /bin/go.ce` | go 语言 REPL/测试通过（语法错误要报行号） |
| `run /bin/ocntools.ce` | ot 工具集菜单（文件管理器/十六进制编辑等） |

### 3.9 网络（预期"硬件缺失"，非缺陷）

| 命令 | 预期 |
|---|---|
| `netcfg` / `netdiag` | 无 NIC 配置时提示 NOT FOUND / 初始化失败 |
| `run /bin/captest.ce` | root 下 `net_init=-1`（硬件缺失）；零能力用户下 `net_init=-13`（权限拒绝）——两者语义不同，都要验 |
| `ping` / `curl` / `dns` / `udp` | 需 `-netdev user,id=n0 -device pcnet,netdev=n0`（QEMU）才可用；VMware 的 vmxnet3/e1000 目前不受支持，属已知限制 |

### 3.10 游戏（综合体验）

| 命令 | 预期 |
|---|---|
| `run /bin/adofai.ce` | 菜单出现；有节奏音效（SB16）；按键（pollkey）响应；ESC 退出干净 |
| `run /bin/demo.ce` / `run /bin/gfxdemo.ce` | 图形演示无花屏 |

---

## 4. 判定标准

- **PASS**：实际输出包含第 3 节所列要点；无 ring0 fault banner；提示符恢复。
- **FAIL**：输出与预期不符、出现 `page-fault` / triple fault / 死机、或命令无响应超过 10 秒。
- FAIL 时记录：命令、完整屏幕输出、`faultinfo` 输出、引导介质与数据盘来源、VMware/QEMU 版本。提交到 `docs/CatOS-内核问题清单.md`（新问题续编号 #107+）。

## 5. 已知非缺陷项（勿报 bug）

| 现象 | 说明 |
|---|---|
| `PCnet NOT found` / net_init=-1 | 未带网卡参数，硬件缺失 |
| 图形模式退出时的 `consent denied/granted` 提示 | 40x12 文本模式退出确认设计 |
| `format` 后 `/bin` 清空 | 内核用内嵌应用兜底重建；预装盘才会满 |
| 引导早期 `file not found`（GRUB） | 用了未打补丁的 `build/catos_work.iso` 模板；请用 `kernel_v25/catos.iso` |
| 磁盘镜像引导卡死 | 数据盘 MBR 无引导代码，本来就不是引导介质 |

## 6. 与自动化回归的对照

系统内手动测试通过后，宿主侧跑全套自动回归（各自独立 monitor 端口）：

```
宿主: test_mkce 48  test_cc 10  test_app_list 24 apps  ce_regress 11P/17S/0F
实机: qemu_regress 12/12  user_regress 15/15  ot_regress ALL  diag_regress 4/4
      disk_regress 5/5  heap_regress 4/4  go_regress 7/7  vprobe_regress 10/10
      snd_regress 6/6  gfx24_regress 0 failed  security_regress 17/17
      e1000_regress 0 failed  es1371_regress 0 failed
实机(-vga vmware): svga24_regress 34/34 PASS（v2.20，端口 4467）
实机(-vga std):    cjk_width_regress 0 failed（v2.21，端口 4493）
```

| 手动测试项 | 对应回归脚本 |
|---|---|
| 3.2 权限/大小写变体 | tests/security_regress.py（17 用例，端口 4476） |
| 3.3 faultinfo/隔离 | tests/diag_regress.py（4463） |
| 3.5 punctest | tests/security_regress.py（glyph-count-6961） |
| 3.5 CJK=2×ASCII 等宽 | tests/cjk_width_regress.py（4493，逐像素） |
| 3.6 音频 | tests/snd_regress.py（4460） |
| 3.4 24bpp 逐像素（Bochs） | tests/gfx24_regress.py（4462） |
| 3.4 VMware SVGA II 专项 | tests/svga24_regress.py（4467，`-vga vmware`） |
| 3.7 vprobe | tests/vprobe_regress.py（4461） |
| 3.1 文件系统 | tests/disk_regress.py |
| 3.2 用户全套 | tests/user_regress.py |

### 3.8 v2.57 鼠标 / 选择 / 回溯键位

| 操作 | 口径（text 与 gfx24 均生效） |
|---|---|
| `Ctrl+↑` / `Ctrl+↓` | 回溯按行滚动，每次恰好 **1 行** |
| `Ctrl+PgUp` / `Ctrl+PgDn` | 整页翻动，每次 **con_rows()-1 行**（可视文本行数），内部为连续单行位移 |
| 滚轮 | 每 notch 回溯 **3 行**（内核路径 `con_scrollback(±3)`）；QEMU 6.2 板载 PS/2 鼠标 HMP 不转发 dz，guest 端滚轮覆盖为 deferred |
| 左键按下拖动 | 按单元格网格选择；松开后所选 UTF-8 文本存入内核剪贴板，选中区反色高亮 |
| 中键 | 把剪贴板粘贴到当前提示符输入行（等价键入，多字节 UTF-8 安全） |
| 右键 | 取消当前选择 |
| `clip` | 无参打印剪贴板内容；`clip -c`（或 `clip clear`）清空 |
| 选择边界 | 可选区止于 `rows-3`；底部两行预留给 IME 候选区，不参与选择 |

回溯实现为增量单行搬移：一次按键 = 整屏行位移一行 + 仅重绘新补入的一行，不做全屏清空整屏重画；到顶/到底边界不重复行、不空行；按任意非回溯键返回活屏并恢复。

## 7. 记录表模板

```
日期:        引导方式: VMware / QEMU    镜像: catos.iso md5=__________
冒烟 S1~S8:  [ ] 全 PASS   失败项: ______
模块测试:    3.1[_] 3.2[_] 3.3[_] 3.4[_] 3.5[_] 3.6[_] 3.7[_] 3.8[_] 3.9[_] 3.10[_]
FAIL 明细:   （命令 / 输出 / faultinfo）
结论:        通过 / 带缺陷通过 / 不通过     测试人: ______
```

---

# 原文：系统内测试文档-2026-11-18.md

# CatOS 系统内测试文档 · 2026-11-18 轮次

> 基线仓库 HEAD：`aae8640`；本文档覆盖本轮新增功能的手工 / 脚本验收步骤与验收点。
> 实现状态说明：本轮鼠标、剪贴板、视频模式持久化、分页器、`keys` 命令等由并行子代理实现；HEAD 基线中上述命令与事件子系统**尚未落地**（全仓 grep 无功能命中），下表“预期结果”为交付口径，“实测结果”列在功能落地、镜像构建完成后逐条回填。
> 自动化对照：
> - `tests/videomode_regress.py` —— 视频模式默认值 / `/etc/videomode` 取值 / 回退；
> - `tests/mouse_regress.py` —— 鼠标光标、滚轮、拖选、中键粘贴、右键取消、`clip`；
> - `tests/pager_regress.py` —— 分页器三键行为、重定向 / 后台不阻塞；
> - IME 两行候选区对应 `docs/设计手册.md` 第三部分 阶段一，自动化脚本 `tests/ime_bar_regress.py` 待补。
> 标注：【自动】= 有 HMP 驱动脚本（`tests/qemu_driver.py` 的 `sendkey / type_text / cmd` 封装）；【手工】= 需人工在 VMware / QEMU 操作。
> 测试环境沿用本指南附录（原 `CatOS-系统内测试文档.md`）第 1 节：QEMU `-m 256`，数据盘 `kernel_v25/catos_disk.img`；引导即 root 会话。

---

## 1. 视频模式（videomode）

> 基线事实：HEAD `render_init()` 默认装载 `be_text`（`render.c:554`），启动后不自动切图形模式；模式切换经 `con_request_mode()` → `switch_backend()`，新后端 `enter()` 失败时还原旧后端（`render.c:514-516`）。本轮目标：开机默认 gfx24，模式选择可由 `/etc/videomode` 持久化。

| # | 类型 | 输入 / 操作 | 预期结果 | 实测结果 | 自动化对应 |
|---|---|---|---|---|---|
| 1.1 | 【自动】 | 冷启动（不输入任何模式命令） | 默认进入 gfx24（vbe24，800×600 24bpp，100×37 网格）；`display` 后端名含 vbe24，网格 100×37，CJK = yes | 待回填 | `tests/videomode_regress.py` |
| 1.2 | 【自动】 | 写 `/etc/videomode` 取值 `text`，重启 | 重启后进入 text 模式 80×25；`display` 显示 vga-text-80x25 | 待回填 | `tests/videomode_regress.py` |
| 1.3 | 【自动】 | 写 `/etc/videomode` 取值 `gfx`，重启 | 进入 Mode 13h，40×12，256 色 | 待回填 | `tests/videomode_regress.py` |
| 1.4 | 【自动】 | 写 `/etc/videomode` 取值 `vbe`，重启 | 进入 32bpp VBE，100×37 | 待回填 | `tests/videomode_regress.py` |
| 1.5 | 【自动】 | 写 `/etc/videomode` 取值 `vbe24`，重启 | 进入 24bpp vbe24，100×37 | 待回填 | `tests/videomode_regress.py` |
| 1.6 | 【手工】 | `/etc/videomode` 写入非法值（如 `foo`）或删除该文件后重启 | 不卡启动；回退默认 gfx24；控制台给出可识别的默认/回退提示或静默使用默认值 | 待回填 | — |
| 1.7 | 【手工】 | 在无 VBE 的环境（或屏蔽 VBE 设备）下请求 vbe / vbe24 | 后端 `enter()` 失败后还原进入前的模式，不黑屏、不 triple fault；提示符可用 | 待回填 | — |
| 1.8 | 【手工】 | 各模式下执行 `display` | 后端名、网格、物理像素、色深、CJK 支持、字形计数、单元格尺寸与当前模式一致；字形计数 = 6961 | 待回填 | — |

---

## 2. 鼠标与剪贴板

> 基线事实：HEAD 无鼠标事件队列、无 HID 鼠标报告处理、无剪贴板 / 选中子系统（`usb_uhci.c` 仅 HID 键盘引导协议）；本节为 v2.57 并行工作流交付口径。

| # | 类型 | 输入 / 操作 | 预期结果 | 实测结果 | 自动化对应 |
|---|---|---|---|---|---|
| 2.1 | 【自动】 | 移动鼠标 | 屏幕上有可见光标并随鼠标移动；光标位置与实际移动方向一致 | 待回填 | `tests/mouse_regress.py` |
| 2.2 | 【自动】 | 滚轮向上一格 / 向下一格 | 回溯视图或正文滚动 3 行 / notch；到顶 / 到底后不再继续滚动，无越界抖动 | 待回填 | `tests/mouse_regress.py` |
| 2.3 | 【自动】 | 左键按下并拖动一段文本后松开 | 拖动途经文本高亮选中；松开后选中态保持；移出可视区域按回溯规则处理 | 待回填 | `tests/mouse_regress.py` |
| 2.4 | 【自动】 | 先拖选一段文本，在输入行光标处按中键 | 选中文本粘贴到当前光标处；UTF-8 多字节字符完整、不截断半个汉字 | 待回填 | `tests/mouse_regress.py` |
| 2.5 | 【自动】 | 选中状态下按右键 | 取消选中 / 清除临时选择态；不向输入行写入内容 | 待回填 | `tests/mouse_regress.py` |
| 2.6 | 【手工】 | `clip`（无参数） | 显示当前剪贴板内容（空则提示空） | 待回填 | — |
| 2.7 | 【手工】 | `clip TEXT` 写入一段文本，再中键粘贴 | 粘贴结果与写入文本逐字节一致；含中文时三字节 UTF-8 完整 | 待回填 | `tests/mouse_regress.py`（写入路径可脚本化部分） |
| 2.8 | 【手工】 | IME 候选区打开期间操作鼠标 | 候选区点选对接属规划阶段三；本轮鼠标选择与既有 shell/编辑器文本操作不互相破坏 | 待回填 | — |

---

## 3. 回溯滚动（Ctrl 组合键）

> 基线事实：HEAD 中 Ctrl+↑/↓ 为 1 行/次，Ctrl+PgUp/PgDn 当前按 `(rows-1)/2` 半屏翻页（`keyboard.c:415-421`）；本轮口径：PgUp/PgDn 页高 = 可视行数。

| # | 类型 | 输入 / 操作 | 预期结果 | 实测结果 | 自动化对应 |
|---|---|---|---|---|---|
| 3.1 | 【自动】 | 输出超过一屏后按 Ctrl+↑ | 回溯视图上滚 1 行 / 次；连续按逐行上移 | 待回填 | `tests/mouse_regress.py` / 回溯用例 |
| 3.2 | 【自动】 | 回溯状态按 Ctrl+↓ | 下滚 1 行 / 次；回到实时视图后停止 | 待回填 | 同上 |
| 3.3 | 【自动】 | Ctrl+PgUp | 上翻一页，页高 = 当前可视行数（`con_rows()`） | 待回填 | 回溯用例 |
| 3.4 | 【自动】 | Ctrl+PgDn | 下翻一页，页高 = 当前可视行数；回到实时视图后停止 | 待回填 | 回溯用例 |
| 3.5 | 【手工】 | 分页器 `-- More --` 提示出现期间按 Ctrl+PgUp/PgDn | 分页状态与回溯互不冲突；具体优先级（回溯接管或分页忽略）按最终实现如实记录，不得丢键 / 死锁 | 待回填 | — |

---

## 4. 通用分页器（v2.59）

> 口径：`help`、`ls` 大目录、`cat` 长文件输出超过一屏时暂停；提示行位于底部；可视行按 `con_rows() - 2` 保守计（预留底部两行给 IME 候选区）。重定向、后台、脚本模式自动关闭分页，不阻塞。

| # | 类型 | 输入 / 操作 | 预期结果 | 实测结果 | 自动化对应 |
|---|---|---|---|---|---|
| 4.1 | 【自动】 | 执行 `help`（输出超一屏） | 底部出现分页提示 `-- More (Enter next line, Space page, q quit) --`；正文按 `con_rows()-2` 行缓冲，不占用底部两行 | 待回填 | `tests/pager_regress.py` |
| 4.2 | 【自动】 | More 状态按 `Enter` | 推进一行；提示仍在，可继续翻 | 待回填 | `tests/pager_regress.py` |
| 4.3 | 【自动】 | More 状态按 `Space` | 推进一屏（页高 = 可视行缓冲行数）；提示仍在直至翻完 | 待回填 | `tests/pager_regress.py` |
| 4.4 | 【自动】 | More 状态按 `q` | 退出分页；剩余输出不再逐屏等待；shell 提示符立即可用 | 待回填 | `tests/pager_regress.py` |
| 4.5 | 【手工】 | `ls /bin`（≥24 个 `.ce`） | 目录条目超屏时触发 More；Enter / Space 可翻完全部条目，无条目丢失 | 待回填 | `tests/pager_regress.py`（目录用例） |
| 4.6 | 【手工】 | `ls /etc` | 列出当前已生成的 `/etc` 条目；条目不足一屏时不出现 More 提示 | 待回填 | — |
| 4.7 | 【自动】 | `help > /tmp/f`（重定向到文件） | 不出现分页暂停；一次性写完；`cat /tmp/f` 内容与未分页完整 `help` 一致 | 待回填 | `tests/pager_regress.py` |
| 4.8 | 【自动】 | 后台执行 `help &` | 不阻塞 shell 提示符；分页器在后台任务中关闭；输出可经 `log` 查看完整内容 | 待回填 | `tests/pager_regress.py` |
| 4.9 | 【手工】 | `cat` 一个长文件（≥两屏） | 超屏触发同一 More 流程；三键行为与 4.2–4.4 一致 | 待回填 | — |
| 4.10 | 【手工】 | 分页期间 Ctrl+PgUp/PgDn 回溯 | 与分页器互不抢占死锁；最终行为按实现如实记录 | 待回填 | — |

---

## 5. F2 中文输入法（两行候选区布局）

> 基线事实：HEAD 为单音节精确匹配 IME，候选条位于输入行下一行（`ime.c:58-64, 66-115`）；本章节对应 `docs/设计手册.md` 第三部分 阶段一的 UI 定案。交互键位：数字选字、Space 首选、`-`/`=` 或 `←`/`→` 翻页、Esc 取消、F2 开关。

| # | 类型 | 输入 / 操作 | 预期结果 | 实测结果 | 自动化对应 |
|---|---|---|---|---|---|
| 5.1 | 【手工】 | shell 提示符下按 F2 | IME 开启；底部出现两行候选区框架；再按 F2 关闭，两行区域还原、不留残影 | 待回填 | `tests/ime_bar_regress.py`（待补） |
| 5.2 | 【手工】 | IME 开启后输入拼音字母（如 `zhong`） | 拼音串显示在输入行光标处（内联），**不进候选区**；退格可删字母 | 待回填 | 同上 |
| 5.3 | 【手工】 | 观察候选区两行布局 | 倒数第二行整行横线铺满可视列；最后一行候选格式为 `1.好 2.号 3.浩`（编号+点+字，空格分隔） | 待回填 | 同上 |
| 5.4 | 【手工】 | 在宽列（vbe24 100 列 / text 80 列）与窄列（gfx 40 列、vbe24 large 50 列）分别触发候选 | 每页候选数按列宽自适应，保底 9 个；宽度允许时放更多；不放半个汉字；窄列先省略候选间空格 | 待回填 | 同上 |
| 5.5 | 【手工】 | 数字键 `1..9` 选字；空格选首选；`-`/`=`、`←`/`→` 翻页；Esc 取消 | 行为与现状一致并新增方向键翻页；Esc 清空拼音串并还原候选区 | 待回填 | 同上 |
| 5.6 | 【手工】 | gfx24（vbe24）模式下重复 5.1–5.5 | 横线行、候选行、汉字绘制正确，无错位 / 截断 | 待回填 | 同上 |
| 5.7 | 【手工】 | text 模式下重复 5.1–5.5 | 候选框架与 ASCII 编号绘制正确；中文缺字形属已知限制（问题清单 #117），不报错 | 待回填 | — |
| 5.8 | 【手工】 | 选中候选上屏后检查输入行 / 编辑器 | 汉字插入光标处，三字节 UTF-8 完整；编辑器内 `^S` 保存后 `cat` 回显一致 | 待回填 | — |
| 5.9 | 【手工】 | 候选多于一页时翻页 | 显示当前页 / 总页数；越界翻页回到第 1 页；空槽位不提交脏字符 | 待回填 | — |
| 5.10 | 【手工】 | `edit` 编辑器内按 F2 输入中文 | 编辑器内开关与上屏正常；退出编辑器时 IME 强制关闭（现状 `shell.c:2345`） | 待回填 | — |

---

## 6. `keys` 命令

> 基线事实：HEAD 无 `keys` 命令（`shell.c` 帮助列表 112–177 行未收录）；本节为新增命令验收口径。

| # | 类型 | 输入 / 操作 | 预期结果 | 实测结果 | 自动化对应 |
|---|---|---|---|---|---|
| 6.1 | 【手工】 | 执行 `keys` | 输出内核识别的特殊键位图例：F1–F12、方向键、Home/End/PgUp/PgDn、Ins/Del、Ctrl+组合键，以及 IME 相关键位（F2、`-`/`=`、`←`/`→`、Esc、Space） | 待回填 | — |
| 6.2 | 【手工】 | 对照实际按键逐一抽查 | 图例所列键位与 `kb_getkey()` 返回码行为一致；无乱码、无缺项、无重复定义 | 待回填 | — |

---

## 7. 本轮回归记录模板

```
日期:        镜像: catos.iso md5=__________   HEAD: aae8640 + 本轮改动
视频模式 1.1–1.8:  [ ] 通过   失败项: ______
鼠标/剪贴板 2.1–2.8: [ ] 通过   失败项: ______
回溯滚动 3.1–3.5:  [ ] 通过   失败项: ______
分页器 4.1–4.10:   [ ] 通过   失败项: ______
IME 候选区 5.1–5.10: [ ] 通过  失败项: ______
keys 6.1–6.2:      [ ] 通过   失败项: ______
自动化: videomode_regress [ ]  mouse_regress [ ]  pager_regress [ ]
FAIL 明细: （命令 / 预期 / 实际现象 / faultinfo）
结论:      通过 / 带缺陷通过 / 不通过     测试人: ______
```

