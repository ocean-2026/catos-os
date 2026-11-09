# CatOS

一个面向 32 位 x86 的实验性 hobby 操作系统内核，附带用户态 CE（Custom Executable）
应用生态。内核以 GRUB El Torito no-emulation ISO 引导启动，用户态应用通过一组
**只增不改（append-only）** 的系统调用槽位与内核通信。

> **文档索引**
> - `docs/` —— 全部设计文档、进度记录、问题清单与历史归档
> - `docs/merge-harvest-PRD.md` —— 「现项目 × 原项目合并取精华」需求分析（合并期权威依据）
> - `docs/merge-harvest-assess.md` —— 合并期评估与执行结论
> - `docs/orig-archive/` —— 原项目（`Catos_Better_better`）文档归档，**不参与构建**
> - `docs/CatOS-内核问题清单.md` —— 已知缺陷与遗留问题追踪
> - `docs/CatOS-v0.3-图形模式成就包.md` —— v0.3 Mode 13h 图形模式成就包（历史记录）

## 目录结构

| 路径 | 说明 |
|---|---|
| `kernel_v25/` | **内核与构建的唯一源码事实**（`src/`、`include/`、`extapp/`、`build/`） |
| `tests/` | CE 格式测试、CE 回归、QEMU 实机回归、宿主侧测试工具 |
| `docs/` | 设计文档、进度、问题清单、归档 |
| `build.sh` | 根目录构建入口（委托给 `kernel_v25/build.sh`） |

## 构建

依赖：GNU 工具链（MSYS2 mingw32 的 `gcc` / `ld` / `objcopy`）、`python`（用于 CE 应用
打包与 ISO 生成）、以及 **已入库的 GRUB 引导模板** `kernel_v25/build/catos_work.iso`。

```sh
sh build.sh kernel   # 仅产出 kernel_v25/build/kernel.bin
sh build.sh iso      # 产出 kernel_v25/build/kernel.bin 与 kernel_v25/catos.iso
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
qemu-system-i386 -cdrom kernel_v25/catos.iso -m 256
```

## 测试

```sh
python tests/test_mkce.py                                        # CE01 格式单元测试
python tests/ce_regress.py                                       # CE 应用回归
python tests/qemu_regress.py kernel_v25/catos.iso --qemu 'C:\qemu-portable\qemu-system-i386.exe'
```

## 许可

仓库内代码与文档仅供学习与研究使用。
