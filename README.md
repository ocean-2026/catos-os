# CatOS

一个面向 32 位 x86 的实验性操作系统内核，目标是向**可通用命令行操作系统**挺进
（路线图见 `docs/ROADMAP-通用操作系统.md`）。内核以 GRUB El Torito no-emulation ISO
引导启动，用户态 CE（CatOS Executable）应用通过一组**只增不改（append-only）** 的
65 槽系统调用表（v2.14，0..64）与内核通信。

## 能力一览

| 领域 | 能力 |
|---|---|
| 进程 | 多任务、spawn/wait、管道、`>`/`>>` 重定向、ELF32 PIE+ASLR 加载器 |
| 文件系统 | FAT32（双 FAT、脏扇区增量写、mount 时镜像自检 + `fsck`）、Unix 式 uid/gid/mode ACL |
| 用户 | 多用户账户（`/etc/passwd`）、7 位能力位（CAP_NET/KILL/FSADMIN/AUDIO/USERADM/DEVICE/SYS）、初始用户 root、`su`/`login`/`useradd`/`chmod`/`chown` |
| 诊断 | 致命故障记录（向量/ring/任务/eip/CR2）+ `faultinfo` 命令、`panic()` 统一入口 |
| 图形 | VGA 文本 + 800x600 24 位真彩，双后端（QEMU/Bochs VBE dispi 24bpp、VMware SVGA II 32bpp XRGB 承载 RGB24），用户态绘图 ABI（gfx_info/rect/blit）；`vbe` / `vbe24` / `gfx24` / `svgainfo` |
| 音频 | Sound Blaster 16（ISA DMA）PCM 驱动 |
| 网络 | 用户态网络栈（curl/netinfo 等应用，能力位门控） |
| 应用 | 22+ 内置 CE 应用（node.js 子集解释器、微型 C 编译器 cc、ocntools 工具箱（文件管理器/十六进制编辑/搜索/版本控制）、adofai 音游、calc、curl…）；应用可从磁盘分发，内核内嵌副本仅作 fallback |

## 文档索引

- `docs/ROADMAP-通用操作系统.md` —— **通用化路线图（行动纲领，先读这个）**
- `docs/PROGRESS.md` —— 最新进度汇报（滚动更新）
- `docs/CatOS-内核问题清单.md` —— 已知缺陷与遗留问题追踪（编号至 #95）
- `kernel_v25/extapp/docs/API.txt` —— CE ABI 系统调用表（v2.14）
- `docs/merge-harvest-PRD.md` —— 「现项目 × 原项目合并取精华」需求分析（合并期权威依据）
- `docs/merge-harvest-assess.md` —— 合并期评估与执行结论
- `docs/orig-archive/` —— 原项目（`Catos_Better_better`）文档归档，**不参与构建**
- `docs/` 其余文件 —— 设计文档与历史记录

## 目录结构

| 路径 | 说明 |
|---|---|
| `kernel_v25/` | **内核与构建的唯一源码事实**（`src/`、`include/`、`extapp/`、`build/`） |
| `kernel_v25/build/apps.list` | 内置应用**单一来源清单**（build.sh 与 gen_builtin.py 共用） |
| `kernel_v25/build/mkdisk.py` | 宿主侧生成预装应用的 FAT32 磁盘（#90） |
| `tests/` | CE 格式测试、CE 回归、QEMU 实机回归、宿主侧测试工具 |
| `docs/` | 路线图、进度、问题清单、设计文档、归档 |
| `build.sh` | 根目录构建入口（委托给 `kernel_v25/build.sh`） |

## 构建

依赖：GNU 工具链（MSYS2 mingw32 的 `gcc` / `ld` / `objcopy`）、`python`（用于 CE 应用
打包与 ISO 生成）、以及 **已入库的 GRUB 引导模板** `kernel_v25/build/catos_work.iso`。

```sh
sh build.sh kernel   # 仅产出 kernel_v25/build/kernel.bin
sh build.sh iso      # 产出 kernel.bin 与 kernel_v25/catos.iso
sh build.sh disk     # 产出预装全部应用的 catos_disk.img（应用与内核解耦，#90）
```

在 Windows（Git Bash）下的推荐调用方式——显式给出 MinGW 路径，避免 `gcc` 被其他
工具链劫持：

```sh
cd kernel_v25 && env -i \
  PATH=/d/msys/mingw32/bin:/usr/bin:/bin HOME=/tmp TMP=/tmp TEMP=/tmp \
  PYTHON=/c/Users/Admin/.workbuddy/binaries/python/versions/3.13.12/python.exe \
  sh build.sh iso
```

ISO 的组装由 `kernel_v25/build/patch_work_iso.py` 完成：它读取 GRUB 模板
`build/catos_work.iso`，把 `kernel.bin` 作为 `kernel.bin;1` 目录项追加进去。
**该模板是构建的必需输入，必须随仓库一起分发**（详见 `.gitignore` 中的说明）。

## 运行

```sh
# 纯 ISO（应用来自内核内嵌 fallback）
qemu-system-i386 -cdrom kernel_v25/catos.iso -m 256

# ISO + 预装磁盘（应用来自磁盘，推荐，#90 之后的主形态）
qemu-system-i386 -cdrom kernel_v25/catos.iso -m 256 -hda kernel_v25/catos_disk.img
```

启动后即以 **root** 登录（提示符 `root@catos:/$`）。常用命令：`help` / `ls` / `cat` /
`cp` / `mv` / `rm` / `run` / `whoami` / `caps` / `faultinfo` / `fsck`；用户管理：
`useradd` / `su` / `login` / `passwd` / `chcaps` / `chmod` / `chown`；进阶工具：
`ocntools`（文件管理器/十六进制编辑/搜索/版本控制）、`adofai`（音游）。

## 测试

```sh
python tests/test_mkce.py          # CE01 格式单元测试（48 例）
python tests/ce_regress.py         # CE 应用回归（宿主模拟器）
python tests/test_app_list.py      # apps.list 单一来源守卫
python tests/qemu_regress.py kernel_v25/catos.iso --qemu 'C:\qemu-portable\qemu-system-i386.exe'
# 其余实机回归：ot_regress / diag_regress / disk_regress / user_regress / vprobe_regress /
#              snd_regress / gfx24_regress / svga24_regress（-vga vmware）/ heap_regress /
#              go_regress（各自独立 monitor 端口）
```

## 许可

仓库内代码与文档仅供学习与研究使用。
