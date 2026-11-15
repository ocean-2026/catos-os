# CatOS 系统内测试文档（v2.22 版）

> 取代 v2.20 版。本版基于 **2026-11-14 17:19 实际引导验证**重写，不是照抄旧条目。
> CE ABI 66 槽（slot 0..65，`gotoxy` 为末槽；`include/ce_abi.h` 文件头注释仍写
> "65 slots"，已过时，别拿注释当准）。
> 测试介质：`kernel_v25/catos.iso`（GRUB isohybrid）
> 数据盘：`kernel_v25/catos_disk.img`（FAT32，宿主 `mkdisk.py` 生成）
> 配套自动化回归：`tests/`；红队报告：`deliverables/engineering-assurance/`

---

## 0. 测试前必查（不查就别开始，结果不可信）

### 0.1 当前镜像身份（2026-11-14 17:19 实测）

| 对象 | 值 | 说明 |
|---|---|---|
| `kernel_v25/catos.iso` | `b483f600fdb1563ff92f0c1c34314ea9` | 17:19 重建 |
| `kernel_v25/build/kernel.bin` | `831227cc10f78b1f19c6f58d4b4193a4` | 6,695,092 B |
| `kernel_v25/catos_disk.img` | `7a246309a98da612b0709f06ad2b83ee` | **11-13 20:08，落后 ISO 21 小时** |
| `git HEAD` | `a92a03e`（v2.22 红队加固） | 全部提交仅本地，未 push |

**当前 ISO 是混合体**：`kernel.bin` 里能搜到 `MARATHON` / `EASY.TAD` / `NORMAL.TAD` /
`HARD.TAD` 字符串，但 4 个 `.TAD`、`builtin_tads_extra.inc`、`adofai.c`、
`gen_builtin.py` 的改动**都还没提交**。也就是说：这份 ISO **无法从 `git HEAD` 复现**。
要出可复现结论，先 `git add` 这些改动或 `git stash` 后重建 ISO。

### 0.2 三个会让你误判的现状陷阱

1. **旧数据盘的 `/bin` 没有 H8 只读密封**。实测在 `catos_disk.img` 副本上执行
   `rm /bin/hello.ce` **直接成功、无任何 read-only 拒绝**，`ls /bin` 里 hello 消失。
   原因：`ce_install_builtins()` 每次开机运行但**跳过已存在文件**，旧盘上的文件是
   v2.21 时代写入的，没带 `FAT_ATTR_RO`。**测 §3.11 之前必须先 `format`（y 确认）
   或删掉 `/bin/*` 后重启**，让安装器以 RO 属性重建。
   `tests/ro_builtin_regress.py` 自己第一步就是 `format`，正是为此。
2. **旧数据盘的 `/bin` 是 8.3 短名，没有 LFN 条目**。实测 `ls /bin` 打印
   `allocpro.ce` 而不是 `allocprobe.ce`。内核 `format` 出来的盘才有长名条目。
   写断言时按盘的来源区分，别照搬 `heap_regress` 的长名断言。
3. **`tests/qemu_driver.py` 的 `cmd()` 是发给 QEMU monitor 的 HMP，不是给 guest
   敲命令**。向 guest 输入必须用 `type_text(s, "...")` + `sendkey(s, "ret")`。
   用错的表现是：屏幕只剩提示符、命令全部"静默消失"。等提示符用
   `wait_for_prompt()`，固定 `sleep` 会丢命令。

### 0.3 端口分配（共享机器，各脚本独占，不得复用）

| 端口 | 脚本 | 端口 | 脚本 |
|---|---|---|---|
| 4444 | `qemu_regress` / `ro_builtin_regress` **默认** | 4467 | `svga24_regress`（`-vga vmware`） |
| 4460 | `snd_regress` | 4468 | `es1371_regress` |
| 4461 | `vprobe_regress` | 4469 | `e1000_regress` |
| 4462 | `gfx24_regress`（`-vga std`） | 4476 | `security_regress` |
| 4463 | `diag_regress` | 4492 | `crash_attack`（红队） |
| 4493 | `cjk_width_regress` | 4494 | `isolation_verify` |
| 4496 | `regression_after_fix` | 4497 | 临时探针（本轮使用） |

`ro_builtin_regress.py` 默认占 4444，与 `qemu_regress.py` 冲突；并发时给它显式
`--port 4495 --disk <副本>`。**绝不** `taskkill /F /IM qemu-system-i386.exe`
（会杀掉全机 QEMU，含他人 guest），一律 `kill_qemu(proc)` 按 PID。

---

## 1. 测试环境准备

| 项 | 方式 |
|---|---|
| VMware | 双击 `vmware/CatOS.vmx`（光驱 El Torito，数据盘 IDE 0:0） |
| QEMU | `C:\qemu-portable\qemu-system-i386.exe -m 256 -cdrom kernel_v25/catos.iso -drive file=<数据盘副本>,format=raw,if=ide -boot order=d -monitor tcp:127.0.0.1:4497,server,nowait` |
| 磁盘镜像路径 | 必须 Windows 绝对路径，QEMU 不认 MSYS `/tmp/...` |
| 登录 | 引导即 root 会话（`whoami` → `root`） |
| 冷启动 | 到 shell 提示符实测 **23.6 秒**，别用固定 5 秒 sleep |

**用数据盘副本测**。`format` / `rm` 会改盘，别拿 `catos_disk.img` 原件做破坏性测试。
`clear` 是清屏命令（不是 `cls`）。

---

## 2. 冒烟测试（约 10 分钟，必过）

| # | 命令 | 预期输出要点 | 失败含义 |
|---|---|---|---|
| S1 | （引导完成） | `CatOS 0.3 -- A tiny Unix-like OS on GRUB Multiboot` + `root@catos:/$` | 引导链/显示故障 |
| S2 | `about` | ASCII logo + `Architecture : i386` + `Bootloader : GRUB 2 (ISO9660 / El Torito)` + `Runs on : QEMU / VMware / Bochs / real PC`。**`about` 不打印版本号**——旧版指南写"版本号"是错的，别据此判 FAIL | 内核镜像陈旧 |
| S3 | `uname` / `uptime` | CatOS 标识 / 秒数+任务数 | — |
| S4 | `ls /bin` | 旧宿主盘实测 **25 个 `.ce`**；`format` 重建后是内嵌的 24 个 | 数据盘未挂载或应用缺失 |
| S5 | `mem` / `hwinfo` | 内存数值 + 堆统计 / 完整硬件转储 | 内存探测异常 |
| S6 | `run /bin/hello.ce` | `hello from ce!` + `[task … exit(0)]` | CE 加载器故障 |
| S7 | `date` | 正确日期时间 | RTC 故障 |
| S8 | `cat /etc/passwd` | root 行可见 | 用户库异常 |
| S9 | `fsize /bin/hello.ce` | `fsize: 4312 bytes`（v2.22 新命令，旧盘实测值） | `fsize` 未编进镜像 |

---

## 3. 分模块测试项

### 3.1 文件系统

| 命令序列 | 预期 |
|---|---|
| `mkdir /tmp/t1` → `touch /tmp/t1/a.txt` → `ls /tmp/t1` | 目录与文件均出现 |
| `cp /bin/hello.ce /tmp/h2.ce` → `ls /tmp/h2.ce` | 存在，0644 归属当前用户 |
| `mv /tmp/h2.ce /tmp/h3.ce` → `ls /tmp` | h2 消失、h3 出现 |
| `cat /tmp/h3.ce` | 二进制乱码可读出 |
| `rm /tmp/h3.ce` → `ls /tmp` | h3 消失 |
| `fsck` | `FAT #2 mirrors FAT #1` |
| `cd /bin` → `ls` → `cd /` | cwd 切换正常 |
| **A13 回归**：对目录执行写操作 | **`-2 EISDIR`**，不是越权写入 |

### 3.2 用户与权限

| 命令序列 | 预期 |
|---|---|
| `whoami` / `id` / `caps` | `root` / uid=0 / 能力位图例 |
| `useradd alice` → `lsuser` / `users` | alice 出现在列表 |
| `passwd alice` → `login alice`（两次口令） | 会话切到 alice |
| `cp /bin/captest.ce /tmp/nox.ce` → `run /tmp/nox.ce` | **err -2**（无 execute bit） |
| `chmod +x /tmp/nox.ce` → `run /tmp/nox.ce` | 正常执行 |
| `chmod 000 /tmp/nox.ce` → `run /tmp/nox.ce` | **err -13** |
| 大小写/路径变体（#97） | `/TMP/NOX.CE`、`/tmp/../tmp/nox.ce`、`/tmp//nox.ce` 全部 **err -13** |
| `su root` → `run /bin/captest.ce` | 原始 syscall 返回码（root 下 `net_init=-1` 是硬件缺失） |
| `run /bin/perms.ce` | **v2.22 三层语义**：内置 app = EACCES / `/etc` 普通文件 root 可写 / `/home` 可写 |

### 3.3 进程与故障诊断

| 命令序列 | 预期 |
|---|---|
| `run /bin/spinner.ce` → `ps` | 任务表出现 spinner |
| `top` / `jobs` / `log TID` | 负载显示（任意键退出）/ 后台任务 / 捕获输出 |
| `kill <tid>` | 目标任务结束；**H1**：ring3 尝试 kill ring0 shell ⇒ **EPERM**，系统不 DoS |
| `run /bin/crash.ce` → `faultinfo` | `page-fault … eip= cr2= err= efl= at tick` + PF bits |
| 故障后 `ps` / `ls` | 提示符恢复，系统存活（隔离性） |
| **H4**：`wait` 非自身子进程 | 立即拒绝，不自锁、不与 shell 互锁 |

### 3.4 显示子系统

| 命令 | 预期 |
|---|---|
| `text` | 回 80x25（退出图形需物理按 `Y` 确认） |
| `gfx` / `vbe` | 真彩文本终端（QEMU std / QEMU vmware / VMware 均可） |
| `gfx24` / `vbe24` | 800x600 诊断图；`vbe24` 是 v2.20 别名 |
| `svgainfo` | VMware：`SVGA II adapter : present`、`id : 0x90000002`、**`updates>0`**；QEMU std 提示无该设备 |
| `display` | `graphics HW` + `last gfx24 hw` / `hw_bpp` / `stride` |
| `gfx24` 后 `text` | 无残影（#57/#100） |
| `vdiag` | 不破坏之前 `ls` 的内容（#100）；`glyphA` 一行内联完整 |
| `palette 4` / `rgb` / `style` / `color` | 即时生效，恢复用 `text` |
| **H5 回归**：`gfx_rect` / `gfx_blit` 超大 x/y 或 `h=0x40000000` | 窗口门控拒绝，无 OOB 写、不崩 |

**光标契约（#109/#110，逐像素已验）**：当前格**无字符** ⇒ 格子末扫描线 1 px
**下划线**（窄格 8 px / 宽格 16 px）；**有字符** ⇒ 该格 **fg/bg 反色**；
**必须闪烁**（0.5 s 半周期）；换行/编辑/滚动后**不留残光标**。
文本模式是 CRTC 硬件光标，扫描线 14–15。

**VMware 专项**：`svgainfo` → `vbe` → `text`+Y → `vbe24` → `text`+Y → `gfx24`，
四条都应 800x600 真彩且**不黑屏**。`updates` 恒为 0 ⇒ 镜像早于 v2.21
（#107 PCD 不可缓存 + #108 IRQ0 直驱 `svga_tick()`/`con_tick()` 两项修复）。

### 3.5 中文与 CJK 字形（#101）

| 命令 | 预期 |
|---|---|
| `run /bin/punctest.ce` | 全角标点表；首行 **`glyphs=6961`**；`、。！？` 为全角，无 `?` |
| `run /bin/zhdemo.ce` | 中文段落正常 |

**等宽逐像素断言**（`tests/cjk_width_regress.py`，端口 4493）：用 UNDERLINE 在格子
末行画的连续段当尺子——纯 ASCII 行 49 格 = **392 px**（8 px/格）；混排行
12 ASCII + 14 CJK = **320 px** ⇒ CJK 步进 **16 px = 2×ASCII**。
退化成 1 格宽会是 224 px、3 格宽 416 px，都会被判失败。

### 3.6 音频

| 命令 | 预期 |
|---|---|
| `sndinfo` | SB16 / ES1371 后端、端口、IRQ、DMA |
| `sndtest 440 800` | `snd: playing 440 Hz for 800 ms` + `irqs=1 busy=0` + **`SNDTEST PASS`** |
| `run /bin/sndtest.ce` | **`SND CE PASS`**（ring3 路径） |
| 无声卡配置 | 静音降级，不报错不挂死 |

### 3.7 内存与堆

| 命令 | 预期 |
|---|---|
| `run /bin/heaptest.ce` / `allocprobe.ce` / `mmaptest.ce` | 全 PASS / found / 往返成功 |
| `run /bin/vprobe.ce fr` | **`VPROBE fr PASS`**（用户缓冲跨页校验，#78）；wr/fw/pw 字符串正确回显 |
| **H2/H3**：`sys_alloc` 超 `USER_HEAP_MAX`、伪造 heap_hdr、double free | 上界拒绝 / 不越界，系统存活 |
| **A05**：`munmap` 的 `addr+len` 32 位回绕 | 拒绝，VMA start 不被改写成 0 |

### 3.8 自举工具链

`run /bin/cc.ce <示例>`（阶段 1-3 编译器）、`run /bin/go.ce`（语法错误报行号）、
`run /bin/ocntools.ce`（ot 工具集菜单）。

### 3.9 网络

`netcfg` / `netdiag` 在无 NIC 时提示 NOT FOUND；`run /bin/captest.ce` 在 root 下
`net_init=-1`（硬件缺失），零能力用户 `-13`（权限拒绝）——两者语义不同，都要验。
QEMU 需 `-netdev user,id=n0 -device e1000,netdev=n0`（或 `pcnet`）才能用
`ping` / `curl` / `dns` / `udp`。**v2.21 起 e1000 `8086:100e` 已受支持**
（旧版文档写"e1000 不受支持"已作废）。

### 3.10 游戏

| 命令 | 预期 |
|---|---|
| `run /bin/adofai.ce` | 菜单、节奏音效、pollkey 响应、ESC 干净退出 |
| `run /bin/demo.ce` / `gfxdemo.ce` | 图形演示无花屏 |

> 当前工作区有 **4 个未提交的新关卡** `EASY.TAD` / `NORMAL.TAD` / `HARD.TAD` /
> `MARATHON.TAD`（72/132/204/200 tiles，BPM 90/120/160/180），`/bin/adofai.ce`
> 与 `builtin_tads_extra.inc` 也未提交。它们**已编进 17:19 的 ISO，但不在 git 里**。
> 测关卡前先确认镜像 md5，否则结果无法回溯到代码。

### 3.11 v2.22 安全加固验证（新增，本轮核心）

**前提：先 `format`（y 确认）或删空 `/bin` 后重启**，见 §0.2 第 1 条。
在旧盘上做这一节会得到"守卫失效"的假结论。

**H8 / A18 — 内置应用只读密封**（`tests/ro_builtin_regress.py`，建议 `--port 4495`）

| 步骤 | 预期 |
|---|---|
| `fsize /bin/hello.ce` | 记下大小（重建后可能与 4312 不同） |
| `cp /bin/hello.ce /tmp/hello.copy` | 成功 |
| `cp /tmp/hello.copy /bin/hello.ce` | **`permission denied`** |
| `edit /bin/hello.ce` | **`read-only`**，且不出现 `CatOS nano` 标题行（若打开需 `^X` 退出） |
| `rm /bin/hello.ce` | **`read-only`** |
| `run /bin/hello.ce` | 仍打印 `hello from ce!` |
| `fsize /bin/hello.ce` | 与步骤 1 **完全相同**（拒绝写入时也没截断/备份） |
| `useradd bob …` / `passwd bob …` / `chcaps` | `/etc/passwd` 仍**可写**（H8 只封 `/bin`，别封死用户库） |

**H9 / A19 — 开机自启动只列不执行**（`tests/autostart_check.py`）

| 检查点 | 预期（17:19 ISO 实测） |
|---|---|
| 引导首屏 | `autostart: banner.sh (not auto-executed; run it manually: run /etc/autoexec/banner.sh)` |
| 首屏 marker | **不得出现** `PWNED_MARKER` |
| `autostart` 命令 | 打印内核内的 `AUTOSTART_BANNER` + 列出脚本名，仍带 `not auto-executed` |
| 往 `/etc/autoexec` 放脚本再重启 | 脚本被列出，**绝不执行** |

**H1/H4/H5/H6 与 A05/A13**（无手敲路径，走 §3.12 攻击样本）：
H1 ring3→ring0 kill 返回 EPERM；H4 wait 仅限自身子进程；
H5 gfx rect/blit 窗口门控 + `clip()` 升 int64；H6 `spawn_load_elf` 拒绝
`pa==0` 未映射页（防 IVT 覆写）；A05 munmap 回绕拒绝；A13 目录写入 `-2 EISDIR`。

### 3.12 红队攻击套件（v2.22 新增）

19 个向量在 `tests/crash_attack.py --list` 里，源码 `extapp/attacks/atk_*.c`，
预编译 `extapp/bin/atk_*.ce`。**这些 CE 不在 `build/apps.list`、不进内嵌镜像、
也不在 `catos_disk.img` 的 `/bin`**（实测 25 个 .ce 里 0 个 atk_*）——
别在系统内手敲 `run /bin/atk_*.ce`，会报找不到文件。走自动化驱动：

```
python tests/crash_attack.py --list                    # 列出 19 个向量
python tests/crash_attack.py --only A01,A05,A08         # 指定子集，端口 4492
python tests/regression_after_fix.py                    # 修复后 7 项回归，端口 4496
python tests/ro_builtin_regress.py kernel_v25/catos.iso --port 4495 --disk <副本>
python tests/isolation_verify.py                       # 10/10 隔离检查，端口 4494
python tests/isolation_verify.py --no-vm               # 仅静态+git+磁盘哈希
python tests/autostart_check.py <iso> <disk> 4497 <label>
```

`crash_attack.py` 会自己用 `craft_bad_ce.py` 造 10 个畸形 CE01 头（A16）、
按用例准备独立磁盘副本，并用 `inject_any.py` 注入。基线与修复后证据分别在
`tests/attack_logs_before_r2/`、`tests/attack_logs_after/`；
判级框架见 `tests/isolation_logs/SEV-FRAMEWORK.md`。

**判定**：7 个已知可利用向量（A06/A08/A09/A10/A13/A18/A19）在修复后 ISO 上
必须全部 `OK`（防线成立），且系统仍回到提示符。修复前 ISO `4dc87877`
有 7/16 向量造成真实破坏，别拿它的日志当本次结论。

---

## 4. 判定标准

- **PASS**：输出包含第 3 节要点；无 ring0 fault banner；提示符恢复。
- **FAIL**：输出不符、`page-fault` / triple fault / 死机、命令 10 秒无响应。
- FAIL 时记录：命令、完整屏幕输出、`faultinfo`、**ISO md5 + 数据盘 md5 +
  `git HEAD` + 工作区是否干净**（当前 ISO 是混合体，缺这几项无法复现）。
- 提交到 `docs/CatOS-内核问题清单.md`，**新问题续编号 #113+**（清单已到 #112）。

## 5. 已知非缺陷项（勿报 bug）

| 现象 | 说明 |
|---|---|
| `no usable NIC` / `net_init=-1` | 未带网卡参数，硬件缺失 |
| `about` 不打印版本号 | 设计如此；版本靠 git HEAD / 镜像 md5 判定 |
| 图形模式退出的 `consent denied/granted` | 退出确认设计 |
| 旧盘 `ls /bin` 显示 `allocpro.ce` | 宿主 `mkdisk.py` 建的盘只有 8.3 条目；内核 `format` 后才有 LFN 长名 |
| 旧盘上 `rm /bin/hello.ce` 成功 | 见 §0.2，盘未刷新；**先 format 再判 H8** |
| 引导早期 `file not found`（GRUB） | 用了未打补丁的 `build/catos_work.iso` 模板 |
| 数据盘引导卡死 | MBR 全零，本来就不是引导介质 |

## 6. 与自动化回归的对照

```
宿主: test_mkce 48  test_cc 10  test_app_list 24 apps  ce_regress 11P/0F
实机: qemu_regress 12/12  user_regress 15/15  ot_regress ALL  diag_regress 4/4
      disk_regress 5/5  heap_regress 4/4  go_regress 7/7  vprobe_regress 10/10
      snd_regress 6/6  security_regress 17/17  e1000/es1371 0F
实机(-vga std):    gfx24_regress 0F(4462)  cjk_width_regress 0F(4493)
实机(-vga vmware): svga24_regress 34/34(4467)
v2.22: crash_attack(4492)  regression_after_fix(4496)  ro_builtin(4495)
       isolation_verify 10/10(4494)  autostart_check(4497)
```

| 手动测试项 | 对应回归脚本 |
|---|---|
| 3.2 权限/大小写变体 | `security_regress.py`（4476） |
| 3.3 faultinfo/隔离 | `diag_regress.py`（4463） |
| 3.4 24bpp / SVGA II / 光标 | `gfx24_regress.py`(4462) / `svga24_regress.py`(4467) |
| 3.5 CJK=2×ASCII | `cjk_width_regress.py`（4493，逐像素） |
| 3.6 音频 | `snd_regress.py`（4460） |
| 3.7 vprobe | `vprobe_regress.py`（4461） |
| **3.11 H8/H9** | `ro_builtin_regress.py`（4495）/ `autostart_check.py`（4497） |
| **3.12 攻击套件** | `crash_attack.py`(4492) / `regression_after_fix.py`(4496) |

## 7. 记录表模板

```
日期:          引导方式: VMware / QEMU
ISO md5:      ________________   kernel.bin md5: ________________
数据盘 md5:   ________________   是否 format 重建 /bin: [ ] 是  [ ] 否
git HEAD:     ________   工作区干净: [ ] 是  [ ] 否（列出未提交项）
冒烟 S1~S9:   [ ] 全 PASS   失败项: ______
模块测试:     3.1[_] 3.2[_] 3.3[_] 3.4[_] 3.5[_] 3.6[_] 3.7[_] 3.8[_]
              3.9[_] 3.10[_] 3.11[_] 3.12[_]
H8/H9:        cp拒绝[_] edit拒绝[_] rm拒绝[_] fsize不变[_] 首屏无PWNED[_]
FAIL 明细:    （命令 / 输出 / faultinfo）
结论:         通过 / 带缺陷通过 / 不通过     测试人: ______
```
