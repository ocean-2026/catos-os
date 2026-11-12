# CatOS 系统内测试文档

> 适用版本：v2.16（内核 ABI 64 slots，`8753f8c` 及以后）
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
| `text` | 回到 80x25 文本模式 |
| `gfx` / `vbe` | 模式列表与 VBE 信息（VMware/QEMU std 均应列出模式） |
| `gfx24` | 切到 800x600x24 并绘制诊断图（红/白/青块） |
| `gfx24` 后 `text` | 无残影回到文本（#100 修复验证点） |
| `vdiag` | 诊断输出 **不破坏**之前 `ls` 的显示内容（#100）；`glyphA` 十六进制一行内联完整（#100） |
| `palette 4` / `rgb` / `style` / `color` | 文本颜色/属性即时生效，恢复用 `text` |
| `display` | 当前显示模式信息 |

### 3.5 中文与 CJK 字形（#101）

| 命令 | 预期 |
|---|---|
| `run /bin/punctest.ce` | 打印全角标点表；首行含 **`glyphs=6961`**；`、。！？`等渲染为**全角宽字形**，无 `?` |
| `run /bin/zhdemo.ce` | 中文段落逐行显示，汉字+标点均正常 |

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
- FAIL 时记录：命令、完整屏幕输出、`faultinfo` 输出、引导介质与数据盘来源、VMware/QEMU 版本。提交到 `docs/CatOS-内核问题清单.md`（新问题续编号 #103+）。

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
```

| 手动测试项 | 对应回归脚本 |
|---|---|
| 3.2 权限/大小写变体 | tests/security_regress.py（17 用例，端口 4474） |
| 3.3 faultinfo/隔离 | tests/diag_regress.py（4463） |
| 3.5 punctest | tests/security_regress.py（glyph-count-6961） |
| 3.6 音频 | tests/snd_regress.py（4460） |
| 3.4 24bpp 逐像素 | tests/gfx24_regress.py（4462） |
| 3.7 vprobe | tests/vprobe_regress.py（4461） |
| 3.1 文件系统 | tests/disk_regress.py |
| 3.2 用户全套 | tests/user_regress.py |

## 7. 记录表模板

```
日期:        引导方式: VMware / QEMU    镜像: catos.iso md5=__________
冒烟 S1~S8:  [ ] 全 PASS   失败项: ______
模块测试:    3.1[_] 3.2[_] 3.3[_] 3.4[_] 3.5[_] 3.6[_] 3.7[_] 3.8[_] 3.9[_] 3.10[_]
FAIL 明细:   （命令 / 输出 / faultinfo）
结论:        通过 / 带缺陷通过 / 不通过     测试人: ______
```
