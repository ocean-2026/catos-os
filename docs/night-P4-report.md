# CatOS 跨夜迭代 P4 分片报告：包管理器框架 + 示例包 + 常用工具迁移

- 基线 commit：`9e47280` v2.37
- 工作副本：`/home/user/Doubao/chats/38453665424926722/night/CatOs-P4`（独立 git 克隆，本地 main，未 push）
- 构建命令：`sh build.sh`（产出 `kernel_v25/catos.iso`）
- 验证方式：QEMU i386（`/home/user/bin/qemu-system-i386`，6.2.0），`-m 256 -no-reboot -net none -cdrom catos.iso -boot order=d -display none -monitor tcp:127.0.0.1:4740`，纯 PS/2 键盘注入。

## 1. 完工判定对照

| 要求 | 状态 | 证据 |
|------|------|------|
| pkg 框架：list / install / remove / info | 完成 | `pkg list` 列出 3 包及状态；`pkg install bc/stats/rps` 解包到 `/bin`；`pkg remove` 删副本；`pkg info` 显示元数据 |
| 宿主侧 mkpkg.py + 设计文档 | 完成 | `kernel_v25/tools/mkpkg.py`；`docs/pkg-repo-design.md` |
| 2–3 个有真实价值的示例包 | 完成 | bc（整数计算器）、stats（行/词/字节统计）、rps（石头剪刀布小游戏） |
| 常用工具迁移 ≥3 个 | 完成 | head、tail、wc、tree（4 个，shell 内建） |
| 实机 install → 运行 → remove 全流程截图 | 完成 | 见 §4 截图清单，全部已 Read 核验 |
| 回归通过 | 完成 | `tests/qemu_regress.py` 11 PASS / 1 WARN（WARN 为基线既有亮度阈值）；`tests/ce_regress.py` 新包 rps/stats PASS |
| 本地 git commit（`P4:` 前缀） | 完成 | 见 §6 |

## 2. 改动文件清单

### 2.1 新增文件

| 文件 | 作用 |
|------|------|
| `kernel_v25/build/pkg.list` | P4 包索引源（`name|version|desc|file|cmd`），单一事实源 |
| `kernel_v25/tools/mkpkg.py` | 宿主侧打包器：CE 应用 + 元数据 → 标准 `.catpkg` 容器 |
| `kernel_v25/extapp/bc.c` | 示例包：非交互整数计算器（`bc 12 + 34`） |
| `kernel_v25/extapp/stats.c` | 示例包：行/词/字节文本统计 |
| `kernel_v25/extapp/rps.c` | 示例包：石头剪刀布一回合 |
| `tests/p4_pkg_test.py` | P4 端到端 QEMU 驱动（格式化 → pkg 全流程 → 工具截图）；私有舞台 `/tmp/catos_p4_stage`，避免并发分片互相覆盖镜像 |
| `docs/pkg-repo-design.md` | 包管理器与未来交叉构建/联网仓库链路设计 |
| `docs/night-P4-report.md` | 本报告 |

### 2.2 修改文件（NIGHT-P4 标记位置，供合并冲突预警）

| 文件 | 改动点（均带 `// NIGHT-P4` 注释） |
|------|------|
| `kernel_v25/build.sh` | 新增 `build_packages()`：读 `build/pkg.list`，编译包 CE 应用并用 mkpkg.py 包成 `.catpkg`；在 `build_apps` 之后、`gen_builtin` 之前调用 |
| `kernel_v25/build/gen_builtin.py` | 读 `pkg.list`；嵌入 `/pkg/*.catpkg` 到 `pkg_files[]`；生成 `pkg_meta[]` 元数据表（name/version/desc/cmd/file） |
| `kernel_v25/build/mkdisk.py` | `STD_DIRS` 增加 `pkg`；预生成磁盘镜像播种 `/pkg/` 容器与 `PKGS.TXT` |
| `kernel_v25/include/ce.h` | 声明 `ce_install_pkg_repo(void)` |
| `kernel_v25/src/builtin_apps.c` | 实现 `ce_install_pkg_repo()`：播种 `/pkg/` 容器 + 写 `PKGS.TXT` 索引（已存在则跳过，不覆盖用户改动） |
| `kernel_v25/src/shell.c` | NIGHT-P4 代码块：`pkg list/info/install/remove`；迁移工具 `head/tail/wc/tree`；help 条目；`shell_exec` 分派 |
| `docs/history.md` | 追加 P4 易错点（容器头宽度、构建顺序、8.3 名、缓冲预留） |
| `docs/experience.md` | 追加 P4 经验（嵌入复用、容器格式、端口隔离） |

### 2.3 构建产物（随源码可重建，已纳入版本控制）

- `kernel_v25/extapp/bin/bc.ce`、`stats.ce`、`rps.ce`
- `kernel_v25/extapp/pkg/bc.catpkg`、`stats.catpkg`、`rps.catpkg`

## 3. 设计要点

### 3.1 本地包仓库（客户机侧）

- 仓库目录：`/pkg/`；索引：`/pkg/PKGS.TXT`，行式 `name|version|desc|file`。
- 包容器 `.catpkg`（定长 140 字节头 + 原始 CE01 负载）：

  | 偏移 | 长度 | 字段 |
  |------|------|------|
  | 0 | 8 | magic `CPKG01\0\0` |
  | 8 | 16 | name |
  | 24 | 16 | version |
  | 40 | 16 | cmd（安装后命令名 → `/bin/<cmd>.ce`） |
  | 56 | 80 | desc |
  | 136 | 4 | payload_len（uint32 LE） |
  | 140 | … | payload（原始 `.ce`） |

- `pkg install`：校验 magic → 长度一致性 → 负载写入 `/bin/<cmd>.ce`（截断替换，ACL 0755）。
- `pkg remove`：删除 `/bin/<cmd>.ce`，保留 `/pkg/` 仓库副本。
- 安装状态由 `/bin/<name>.ce` 是否存在推导，无需额外状态文件。

### 3.2 嵌入与播种链路

`build.sh` → 编译包 CE 应用 → `mkpkg.py` 包成 `.catpkg` → `gen_builtin.py` 嵌入内核镜像（`pkg_files[]` + `pkg_meta[]`）→ 开机/格式化时 `ce_install_pkg_repo()` 播种到 FAT 卷 `/pkg/` 并写 `PKGS.TXT`。

### 3.3 示例包

| 包 | 命令行 | 实机输出（截图核验） |
|----|--------|------|
| bc | `bc 12 + 34` | `12 + 34 = 46` |
| stats | `stats /tmp/sample.txt` | `4 15 100  /tmp/sample.txt` |
| rps | `rps paper` | `you: paper  cpu: paper  -> draw!` |

### 3.4 迁移工具（shell 内建，≥3 个）

| 工具 | 用法 | 实现要点 |
|------|------|------|
| head | `head [-n N] FILE` | 默认 10 行；整文件读入 48 KiB 静态缓冲，按换行截断 |
| tail | `tail [-n N] FILE` | 默认 10 行；反向扫描定位起点 |
| wc | `wc [-l] [-w] [-c] FILE` | 默认全部三项；标志可组合 |
| tree | `tree [DIR]` | `fs_enum_entries` 递归，深度上限 6，跳过 `.`/`..` |

## 4. 截图清单（绝对路径，均已 Read 核验内容）

目录：`/home/user/Doubao/chats/38453665424926722/night/CatOs-P4/tests/qemu_shots_p4/`

| 步骤 | 文件 | 核验内容 |
|------|------|------|
| pkg list（初始） | `p4_01_pkg_list.png` | 3 包均 `available`：bc/stats/rps |
| pkg info bc | `p4_02_pkg_info_bc.png` | name/version/desc/file/status 齐全 |
| pkg install bc | `p4_03_install_bc.png` | `installed bc -> /bin/bc.ce` |
| bc 运行 | `p4_04_bc_run.png` | `12 + 34 = 46`，task exit(0) |
| pkg install stats | `p4_05_install_stats.png` | `installed stats -> /bin/stats.ce` |
| 生成样本文件 | `p4_06_make_sample.png` | `hello > /tmp/sample.txt` |
| stats 运行 | `p4_07_stats_run.png` | `4 15 100  /tmp/sample.txt` |
| pkg install rps | `p4_08_install_rps.png` | `installed rps -> /bin/rps.ce` |
| rps 运行 | `p4_09_rps_run.png` | `you: paper  cpu: paper  -> draw!` |
| wc | `p4_10_wc.png` | `4 15 100  /tmp/sample.txt`（与 stats 一致） |
| head | `p4_11_head.png` | `head -n 5 /appdevelop/API.TXT` 前 5 行 |
| tail | `p4_12_tail.png` | `tail -n 3 /appdevelop/API.TXT` 末 3 行 |
| tree | `p4_13_tree.png` | 递归列出 `/pkg/`：bc/stats/rps.catpkg + pkgs.txt |
| pkg remove bc | `p4_14_remove_bc.png` | `removed bc` |
| 卸载后 list | `p4_15_list_after_rm.png` | bc 回到 `available`，stats/rps `installed` |

## 5. 测试命令与结果

### 5.1 构建

```sh
cd kernel_v25 && sh build.sh iso
# 结果：OK -> catos.iso（11,546,624 字节，含 39 内置 + 3 包）
```

### 5.2 P4 端到端驱动

```sh
python3 tests/p4_pkg_test.py --qemu /home/user/bin/qemu-system-i386 --port 4740
```

流程：格式化 FAT32 → `pkg list` → `pkg info bc` → `pkg install bc` → `bc 12 + 34` → `pkg install stats` → `hello > /tmp/sample.txt` → `stats /tmp/sample.txt` → `pkg install rps` → `rps paper` → `wc` → `head` → `tail` → `tree /` → `pkg remove bc` → `pkg list`。全部步骤截图留存，人工 Read 核验文本正确。

### 5.3 基线回归

```sh
python3 tests/qemu_regress.py --qemu /home/user/bin/qemu-system-i386
```

结果：`steps=12 ok=11 warn=1`（WARN 为 `boot-shell` 亮度 0.01 的既有阈值，非新回归）。

`tests/ce_regress.py`：新包 `rps.ce`、`stats.ce` PASS；既有 FAIL（netinfo/sndtest 的 ce_sim 不支持 opcode 0x6a）在基线 9e47280 上同样存在，非本次引入。

## 6. Git 提交

- 分支：本地 `main`（未 push）
- 提交信息前缀：`P4: `
- 工作副本改动已按 §2 清单落盘；构建产物 `.ce`/`.catpkg` 一并纳入版本控制（与基线惯例一致）。

## 7. 遗留问题与后续

1. **联网拉包未实现**：内核 e1000 驱动与 `net_send`/`net_recv` 接口已在位，但 P4 只做本地 `/pkg/` 仓库；HTTP/1.0 客户端、仓库索引拉取、签名校验留待后续（见 `docs/pkg-repo-design.md` §2）。
2. **依赖解析/升级/回滚未实现**：当前 `pkg install/remove` 无依赖拓扑；升级与回滚是设计预留。
3. **大文件工具缓冲**：`head/tail/wc` 整文件读入 48 KiB 静态缓冲（bss，不占镜像）；超过 48 KiB 的文件会被拒绝（提示 `file too large`）。CatOS 现有文档/应用均在此范围内。
4. **包名 8.3 约束**：FAT32 无 LFN，包名/命令名需 stem≤8、ext≤3；当前 bc/stats/rps 均满足。
5. **ce_sim 既有 FAIL**：netinfo/sndtest/mmaptest/heaptest 在模拟器下报不支持 opcode 0x6a 或 `ff /6 not emitted`，基线复现，与本次改动无关；新包 `rps.ce`、`stats.ce` 模拟器 PASS。

### 7.1 本轮排障记录（已修复）

- **现象**：一轮实机截图中 `pkg/stats/rps/wc/head/tree` 全部 `unknown command`，但本地 `kernel.bin`/`catos.iso` 用 `strings` 都能查到新命令串。
- **根因**：`qemu_driver._stage_image()` 把镜像复制到共享目录 `/tmp/catos_qemu_stage`（文件名固定 `catos.iso`/`qemu_disk.img`），并行分片并发构建互相覆盖；本分片 QEMU 启动了别人覆盖进来的陈旧内核。
- **修复**：`tests/p4_pkg_test.py` 改为私有舞台 `/tmp/catos_p4_stage`，iso 与磁盘两份都私有复制。
- **附带约束**：舞台目录不能放在工作区 hpvs_fs（`/home/user/...`），该文件系统不支持 flock，QEMU 打开镜像即 `Failed to get "write" lock`（rc=1）；私有目录也必须落在 `/tmp`（overlay，支持 flock）。

## 8. 合并冲突预警（新增命令集中位置）

shell 命令分派集中在 `kernel_v25/src/shell.c` 的 `shell_exec()`：

- 新增内建命令：`pkg`、`head`、`tail`、`wc`、`tree`（分派条目带 `// NIGHT-P4` 注释）。
- help 条目在 `cmd_help()` 文件系统段，同样带 `// NIGHT-P4`。
- 包仓库播种在 `src/builtin_apps.c` 的 `ce_install_pkg_repo()`，开机与格式化后各调用一次。
- 构建链路：`build.sh` 的 `build_packages()`；`gen_builtin.py` 的 `PACKAGES/PKG_FILES` 表；`mkdisk.py` 的 `/pkg/` 播种。
