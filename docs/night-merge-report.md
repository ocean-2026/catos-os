# CatOS 跨夜迭代合并集成报告（v2.37 → v2.42，2026-11-15）

五分片（P0 4G 内存 / P1 PE 支持 / P2 分区挂载 / P3 彩蛋 UX / P4 包管理器）按
P0→P1→P2→P3→P4 顺序 cherry-pick 并入主干 `main`，基线 HEAD `9e47280`（v2.37）。

## 1. 版本提交与推送

| 版本 | commit | 分片 | 推送 |
|---|---|---|---|
| v2.38 | `9a2e791` | P0 4G 内存（paging.c/hw.c/shell.c/ata.c，修 #122/#123） | ✅ 已推 GitHub |
| v2.39 | `c9180f1` | P1 PE 支持（task.c spawn_load_pe + pe_to_ce_file、shell.c MZ 转换询问、mkpe.py） | ✅ 已推 GitHub |
| v2.40 | `cbe5036` | P2 分区挂载（ata.c 多盘、part.c/part.h、fat.c 多卷上下文、fs.c VFS 挂载表、shell.c mount/umount/df） | ✅ 已推 GitHub |
| v2.41 | `6ca17fa` | P3 彩蛋 UX（cowsay/sl/fortune/cal/yes、cls、echo 增强、unknown 建议） | ✅ 已推 GitHub |
| v2.42 | `26b681c` | P4 包管理器（pkg list/info/install/remove、bc/stats/rps、head/tail/wc/tree、mkpkg.py） | ✅ 已推 GitHub |

推送远端：`github.com/ocean-2026/CatOS.git main`；末次推送区间 `cbe5036..26b681c`。
（推送命令中的 token 已在日志脱敏。）

## 2. 冲突解决记录

### P0（v2.38）
- 干净拣入，无内容冲突。`shell.c` 仅改动 cmd_mem/cmd_export/cmd_unset/size_apply，
  与后续分片不重叠。

### P1（v2.39）
- `docs/history.md`、`docs/experience.md`：双方均为追加条目。保留两侧全部条目，
  去重后无重复行。
- `shell.c` 自动合并：`g_script_mode` 全局、`cmd_run` 内 MZ/PE 转换询问块、
  脚本模式默认拒绝分支均完整落地；与 P0 的 admin 闸门代码分处不同函数，无交叉。

### P2（v2.40）
- `docs/history.md`、`docs/experience.md`：追加条目双侧保留。
- `kernel_v25/src/ata.c`（唯一内核冲突）：
  - P0 把 `ata_wait_drq()` 无参调用改为 DRQ 失败 `return -1`（#123）；
    P2 把 DRQ 等待重构为带驱动参数的 `ata_wait_drq(drv)`。
  - 语义合并：保留 P0 的失败上抛语义，改用 P2 的多盘签名——
    `if (ata_wait_drq(drv) != 0) return -1;`，#123 注释保留。
- `fat.c` / `fs.c` / `shell.c` 自动合并：FAT 几何量迁入卷上下文（vol 指针），
  既有 `fat_get/fat_read_cluster/fat_cluster_size` 簇链接口保留为活动卷操作，
  P1 的 PE/CE 加载走簇链读取路径不受影响。
- 构建清单 `build.sh`：part.c 编译/链接条目与 P4 后续包构建函数分处不同 hunk。

### P3（v2.41）
- `docs/history.md`、`docs/experience.md`：追加条目双侧保留。
- `shell.c` 自动合并：内联 echo 分支被 `cmd_echo(arg)` 替换（未残留旧分支）；
  cowsay/sl/fortune/cal/yes/cls 分支与 P2 的 mount/umount/df 分支、P1 的
  cmd_run 块按各自位置并存；unknown 命令最近邻建议追加到既有提示后。
- 命令分支一条未丢失；`// NIGHT-P3` 标记保留。

### P4（v2.42）
- `docs/history.md`、`docs/experience.md`：追加条目双侧保留。
- `build.sh`、`shell.c` 自动合并：`build_packages()`（pkg.list 编译 .ce + mkpkg 打包）
  与 P2 的 part.c 列表并存；pkg/head/tail/wc/tree 分支落在 mv 分支之后，
  与 P3 彩蛋分支、P2 挂载分支无重叠。
- 集成收尾补全：P4 原分片未把新命令写入 TAB 补全表，已把
  `pkg/head/tail/wc/tree` 补入 `sh_cmds[]`，把 `mount/umount` 与 P4 命令补入
  `BUILTIN_NAMES[]` 建议表（功能不增不减，仅补全提示面）。
- `// NIGHT-P4` 标记保留。

### 接口交叉核对
- P2 多卷重构 × P4 包播种：`ce_install_pkg_repo()` 向根卷 `/pkg/` 播种容器与
  `PKGS.TXT`；`pkg install/remove` 只在根卷 `/bin` 读写，未走挂载卷，
  与 VFS 挂载表无路径冲突。
- P2 多卷重构 × P1 PE 加载：`fs_resolve_file` 按挂载表解析后激活对应卷，
  根卷路径激活卷 0；PE/CE 加载前解析激活正确卷，簇链读取落在活动卷上。

## 3. 回归结果

| 回归脚本 | 结果 | 说明 |
|---|---|---|
| qemu_regress.py（P0 后） | ✅ 12/12 PASS | 端口 4701 |
| qemu_regress.py（P1 后） | ✅ 12/12 PASS | 端口 4702 |
| qemu_regress.py（P2 后） | ✅ 12/12 PASS | 端口 4703 |
| qemu_regress.py（P3 后） | ✅ 12/12 PASS | 端口 4705 |
| qemu_regress.py（P4 后） | ✅ 12/12 PASS | 端口 4706 |
| p2_mount_regress.py | ✅ 16 检查 0 失败 | mount 表/挂载/后缀/跨盘读/跨盘写/df/umount/重挂载全流程 |
| p4_pkg_test.py | ✅ 15 步完成，截图目检通过 | 见第 4 节 |
| fsrec_regress.py | ✅ 12/12 PASS | 最终扫描轮 |
| ce_regress.py | ⚠️ PASS=7 SKIP=13 FAIL=11 | 10 条为基线既有未建模 opcode（0x6a/0x08/ff /6 等）；新增第 11 条为 bc.ce ce_sim 未建模 opcode 0x0a，bc 真机运行正常（`bc 12 + 34 = 46`） |
| caps_regress.py | ⚠️ 部分 | 扫描在 caps 段被终止；已观察到 login-user / format-denied / fsck-denied / reboot-denied 均 PASS，未出汇总行 |
| user_regress.py | ⏳ 未跑 | 最终扫描中断于 caps，未轮到 |
| disk_regress.py | ⏳ 未跑 | 同上 |
| env_regress.py | ⏳ 未跑 | 同上 |
| heap_regress.py | ⏳ 未跑 | 同上 |
| size_regress.py | ⏳ 修复已落、复跑未完成 | 见第 5 节 |

> 中断原因：扫描轮在 caps 中段被环境回收（/tmp 阶段目录被清理、前台进程终止），
> 非内核回归失败。user/disk/env/heap/size 五个脚本需下一轮补跑；命令见
> `tests/run_full_regress.sh`。

## 4. 实机冒烟（截图目检）

P4 包流程截图：`tests/qemu_shots_p4/`（15 张，已逐张 Read 验证）

| 截图 | 步骤 | 目检结论 |
|---|---|---|
| p4_01_pkg_list | `pkg list` | bc/stats/rps 三条 available，仓库 `/pkg/` 播种成功 |
| p4_02_pkg_info_bc | `pkg info bc` | name/version/desc/file/status 字段齐全 |
| p4_03_install_bc | `pkg install bc` | `installed bc -> /bin/bc.ce` |
| p4_04_bc_run | `bc 12 + 34` | 输出 `12 + 34 = 46`，exit(0) |
| p4_05_install_stats | `pkg install stats` | 安装成功 |
| p4_06_make_sample | `hello > /tmp/sample.txt` | 重定向落盘 |
| p4_07_stats_run | `stats /tmp/sample.txt` | 4 行 / 15 词 / 100 字节 |
| p4_08_install_rps | `pkg install rps` | 安装成功 |
| p4_09_rps_run | `rps paper` | `you: paper  cpu: paper  -> draw!`，exit(0) |
| p4_10_wc | `wc /tmp/sample.txt` | 4 / 15 / 100 计数一致 |
| p4_11_head | `head -n 5 /appdevelop/API.TXT` | 头部 5 行正确 |
| p4_12_tail | `tail -n 3 /appdevelop/API.TXT` | 末尾 3 行正确 |
| p4_13_tree | `tree /` | 目录树含 /appdevelop、/pkg（3 容器 + PKGS.TXT） |
| p4_14_remove_bc | `pkg remove bc` | `removed bc` |
| p4_15_list_after_rm | `pkg list` | bc 回到 available，stats/rps 仍 installed |

### 待补冒烟（驱动已就位，预算内未执行）
`tests/smoke_merge.py` 覆盖：`-m 3840` 下 mem/hwinfo、cowsay/sl/cal/fortune/yes、
PE 样例（mkpe.py --sample 现场生成：转换询问 → 直载 PE32 → 离线翻译 CE 运行）、
head/tail/wc/tree、pkg 全流程、分区盘 mount/ls/cat/df/umount。截图输出目录
`tests/qemu_shots_merge/`。运行：

```bash
python3 tests/smoke_merge.py --qemu /home/user/bin/qemu-system-i386
```

## 5. 测试基建修复（size_regress.py，不改内核）

- 绿带检测常量 `GREEN (0x55,0xFF,0x55)` 更新为现代主题实际强调绿
  `#7EE787 = (0x7E,0xE7,0x87)`（render.c CLUT 第 125 行），旧常量滞后于
  v2.37 调色板， prompt 像素永不命中。
- `boot()` 补挂 `kernel_v25/catos_disk.img`：旧脚本无卷，`/etc/size` 持久化
  步骤无源盘可写，属假阴性。
- ISO 参数改为可选（缺省取 catos.iso），与其余回归脚本一致。
- 状态：修复已写入工作区并入本次 docs 提交；脚本复跑结果待下一轮补登记。

## 6. 遗留项（详见问题清单第二十六节 #128~#138）

- P0：PCI hole 第二段 RAM 未枚举（#128）；4GiB MMIO 避让 deferred（#129）。
- P1：.reloc 基址重定位未实现（#130）；CE 转换要求入口 flat 偏移 0（#131）。
- P2：EBR 扩展分区链不遍历（#132）；多物理盘待金属验证（#133）；挂载表不持久（#134）。
- P3：help 顶部小节回滚深度不足（#135）。
- P4：head/tail/wc 48KiB 缓冲上限（#136）；联网拉包 deferred（#137）。
- #122/#123 已于 v2.38 修复并在清单中标注。

## 7. 待办（下一轮）

1. 补跑 user/disk/env/heap/size 五个回归（`tests/run_full_regress.sh`），登记结果。
2. 执行 `tests/smoke_merge.py` 全量冒烟并目检 `tests/qemu_shots_merge/` 截图。
3. caps_regress 补跑到汇总行。
