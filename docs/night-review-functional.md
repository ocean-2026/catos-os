# CatOS v2.38~v2.43 跨夜合并 —— 第一审（功能 / 正确性）审查报告

- 审查轮次：双审闭环第一审（功能/正确性），第二审（审美）另起
- 基线：main HEAD `b2c7251`（v2.43）；审查后版本 **v2.44**（提交 hash 见 `git log` 首条 v2.44）
- 环境：QEMU i386，纯 PS/2（未加 `-usbdevice keyboard`），磁盘挂 IDE；内存两档 `-m 256`（常规档）与 `-m 3840`（4G 档替代，宿主 cgroup ~3.9 GiB 挡 4096 档，属已知环境限制）
- 证据要求：源码核对 + 实机探针 + 边界构造三重；所有实机结论均有 `tests/audit_logs/` 日志与 `tests/qemu_shots_review/` 截图可溯
- 已知 deferred（#128~#141）不计入本轮缺陷

## 1. 总结论

**六大新功能全部通过功能/正确性审查。** 上一轮遗留的 4 个问题经源码复核全部真实存在，已在 v2.44 修复并重编译、实机复现验证；审查中另发现 1 个同类挂载边界（根分区被二次绑定）一并修复（#146）。全量回归基线未被打破：qemu / fsrec / caps / user / disk / env / heap / size 八组 QEMU 回归全绿；ce_regress 仍为既有基线 `PASS=7 SKIP=13 FAIL=11`（ce_sim 未建模 opcode，真机对应应用正常）。

| 功能片 | 审查结论 | 备注 |
|---|---|---|
| P0 4G 内存 | ✅ PASS | 两档 allocprobe 前后 PMM 空闲帧精确相等，无泄漏 |
| P1 PE/EXE 三路径 | ✅ PASS | 4 类非法 PE 全部明确报错不崩溃；样例 PE 直载 + PE→CE 往返正常 |
| P2 分区挂载 | ✅ PASS | 非空/占用/重复挂载/未挂载卸载/重挂持久化/df 数值全部正确；根分区二次绑定已加固 |
| P3 彩蛋 | ✅ PASS | 异常参数（负数年份、非数字、空消息、超长文本、Ctrl+C）均不崩溃 |
| P4 pkg 包管理器 | ✅ PASS | 列表/信息/安装/覆盖重装/卸载/未安装/不存在包名闭环正常 |
| 工具迁移 tree/head/tail/wc | ✅ PASS | 不存在文件、二进制、空文件行为正确 |
| 五片合并命令面 | ✅ PASS | 分发表 / help / TAB 补全三处齐备（详见 §7） |

## 2. 上一轮 4 个已知问题：验证结论与修复

### #144 `tool_read_whole` 1 字节越界写 + 空文件误判（head/tail/wc 共用）

- **源码确认（HEAD b2c7251）**：`tool_buf[TOOL_BUF_MAX]`（49152），当文件恰好 49152 字节时 `tool_buf[rd] = 0` 写在索引 49152，越界 1 字节；同时 `fs_read_at` 在 `max=0` 时返回 -1，工具把 `rd<=0` 一律判为读取失败，空文件被报成 `no such file`。
- **修复（v2.44，`shell.c`）**：
  - 缓冲改为 `tool_buf[TOOL_BUF_MAX + 1]`，留尾 NUL 位；
  - `fs_resolve_file` 成功且 `sz == 0` 直接返回 0（空缓冲），仅 `rd < 0` 报错；
  - 读满路径 `tool_buf[rd] = 0` 不再越界。
- **实机验证**：`tests/review_fix_verify.py`（日志 `tests/audit_logs/review_fix_verify.log`）—— touch 空文件后 `wc` 输出 `0 0 0`，`head`/`tail` 空文件不报错不崩溃；截图 `V_completion.png`。
- **回归**：fsrec / heap / size 回归全绿，未引入新故障。

### #142 同一分区可挂载到两个挂载点

- **源码确认**：`fs_mount` 只检查挂载点占用与目录非空，未按分区归属去重；两个卷槽打开同一 LBA，各自缓存 FAT，写入后两份缓存可能分叉。
- **初版修复**：挂载前扫描 `g_mnt[]` 同设备名拒绝，返回 `-15`（`mount: partition already mounted elsewhere`）。
- **审查追加（#146）**：初版去重只覆盖附加卷槽，根分区（卷槽 0）仍可被二次绑定到另一挂载点，风险同类。v2.44 新增 `fat_volume_owns(drive, lba)`（`fat.c`/`fat.h`），遍历全部卷槽（含槽 0）按「ATA 驱动号 + 分区 LBA」判归属；`fs_mount` 据此拒绝。
- **实机验证**：
  - `tests/audit_logs/review_fix_verify.log`：P2 二次挂载被拒，挂载表仅一条 `hd0p2 /docs`；截图 `V_mount_dedup.png`。
  - `tests/review_root_remount.py`：`mount 1 /docs2`（根 P1 重绑）被拒；`mount 2 /docs` 正常；`mount 2 /docs3` 重复 P2 被拒；`mount` 表只剩 `hd0 /` 与 `hd0p2 /docs`，4/4 PASS。

### #145 TAB 补全表漏收新命令与 df

- **源码确认**：`sh_cmds[]` 只并入了 P4 工具，P3 彩蛋（cowsay/sl/fortune/cal/yes）与 `df` 漏收；help 行与建议表 `BUILTIN_NAMES` 都在。
- **修复（v2.44，`shell.c`）**：补入 `cal/cowsay/df/fortune/sl/yes`，并顺带补收别名 `cls`/`su`，使补全表与建议表对齐。
- **实机验证**：`review_fix_verify.py` 实测 TAB `cows`→cowsay、`df` 可补全；`review_round2.py` 实测 `cl`→候选栏 `clear cls`、`su`→`su sudo`，均 PASS。

### #143 `cal` 月历表头/单元格残留与个位杂符

- **源码确认**：年视图三个月块复用同一组 7 行栈行，只清首字节，上一组月的数字残留；单日个位用宽度 2 `itoa` 补零，把 `'0'` 写进个位格，渲染成 `?`/`?,` 杂符。
- **修复（v2.44，`shell.c` `cal_build_month`）**：7 行 ×22 列整体清零；单日左补空格、改用宽度 0 `itoa`，十位留空。
- **实机验证**：截图 `V_cal_2026_fixed.png`——Oct/Nov/Dec 2026 三栏干净，个位左对齐，表头 `Su Mo Tu We Th Fr Sa` 完整，无 `?`/杂符；`cal -5`、`cal abc` 均被年份区间检查拒绝。

## 3. P0 4G 内存审查

**源码路径**：`hw.c`（`mem_lower/mem_upper`、`HW_MEM_FREE_KB`）、`paging.c`（PMM 位图 `frame_alloc/frame_free`、任务空间回收 `task_space_destroy`）、`shell.c cmd_mem`。

| 探针 | 档位 | 结果 |
|---|---|---|
| `mem` 常规档 | -m 256 | Upper 260992 KB，Total ~255 MB，PMM 空闲 59519 帧 |
| `mem` 内存档 | -m 3840 | Upper 3144576 KB，Total ~3071 MB，PMM 空闲 780415 帧 |
| allocprobe 前后帧计数 | -m 256 | before=59519，after=**59519**，精确回收 |
| allocprobe 前后帧计数 | -m 3840 | before=780415，after=**780415**，精确回收 |
| allocprobe 行为 | 两档 | `alloc(64)`/`alloc(8k)` 地址均在堆窗口 `0x02100000..+512K`，两块不重叠，8k 块末字节读写回环 OK，free 后再分配 128 OK，`ALLOCPROBE PASS`，exit(0) |

- **泄漏判定**：任务退出时 `task_space_destroy` 遍历页表释放所有映射帧；两档探针前后空闲帧差为 0，无内核侧泄漏。
- **证据**：`tests/audit_logs/night_review_probe.log`（C 段）、`tests/audit_logs/review_round2.log`（C/D 段）；截图 `C_allocprobe.png`、`C_mem.png`。
- **结论**：✅ PASS。

## 4. P1 PE/EXE 三路径审查

**源码路径**：`task.c spawn_load_pe` / `pe_to_ce_file`、`shell.c cmd_run` MZ 询问、`tests/mkpe.py`（主机侧翻译，常数从 `ce.h` 读取避免漂移）。

**构造的非法样本**（`tests/review_assets/gen_bad_pe.py`，落盘同目录）：

| 样本 | 构造 | 预期 | 实机结果 |
|---|---|---|---|
| BADMZ.EXE | MZ 头 + `e_lfanew=0`，无 PE 签名 | 拒绝，不崩溃 | ✅ `cannot load (err -5)` |
| BADA_LF.EXE | `e_lfanew=0xFFFFFFF0`，越出文件 | 拒绝 | ✅ `cannot load (err -5)` |
| TRUNC_PE.EXE | 节表偏移超出 EOF | 拒绝 | ✅ `cannot load (err -5)` |
| OVERSEC.EXE | 节原始数据超出 EOF | 拒绝 | ✅ `cannot load (err -5)` |
| PESAMPLE.EXE | mkpe `--sample` 最小可运行 PE32 | 直载 + 往返 | ✅ `PE->CE roundtrip OK`，exit(0) |

**校验链源码核对**：DOS 头大小 → `e_lfanew` 上下界 → PE 签名 → COFF 机器类型 i386 → 非 DLL → 可选头魔数 0x10b → ImageBase 低位 → 镜像落在用户 arena → 节数 ≤16 → 节表/原始数据边界 → 入口偏移非 0；`fat_pread` 有 `off+len > fsize` 越界保护。MZ 询问仅在 `psize≥2` 且读到 MZ 时触发，1 字节/空文件不会误入。

- **证据**：截图 `A_pe_badmz.png`、`A_pe_bada_lf.png`、`A_pe_trunc_pe.png`、`A_pe_oversec.png`、`A_pe_sample.png`；日志 night_review_probe.log A 段。
- **结论**：✅ PASS。非法 PE 全部给出明确错误码回到 shell，无崩溃、无越界解引用。

## 5. P2 分区挂载审查

**源码路径**：`part.c`（MBR 扫描/去重）、`ata.c`（四槽 IDENTIFY）、`fat.c`（多卷槽/激活/释放）、`fs.c`（VFS 挂载表/路径归属/df）。

| 边界探针 | 结果 | 证据备注 |
|---|---|---|
| 挂载点非空（`mount 2 /bin`） | ✅ 拒绝 `target directory is not empty` | B 段 |
| cwd 占用挂载点（`cd /docs` 后 `umount /docs`） | ✅ 拒绝 `busy (current directory is inside)` | B 段 |
| 重复挂载同一分区（`mount 2 /docs2`） | ✅ 拒绝 `partition already mounted elsewhere` | #142 |
| 根分区二次绑定（`mount 1 /docs2`） | ✅ 拒绝（#146 加固） | review_root_remount.py |
| umount 未挂载目录（`/docs2`、`/noent`） | ✅ `not a mount point` | B 段 |
| umount 再 mount 数据持久 | ✅ 重挂后 `note.txt`、`hello.ce` 仍在 | `B_persist.png` |
| 跨卷写/读 | ✅ `/docs/hello.ce` 落盘可读回 | B 段 |
| df 数值 | ✅ used+free=total：hd0 480+13812=14292K；hd0p2 12+16324=16336K | `B_df.png` |
| 挂载表列举 | ✅ `hd0 /`、`hd0p2 /docs (B:)` | `B_mount_list.png` |

- **修复说明**：去重判定已从设备名扫描升级为 `fat_volume_owns(drive,lba)` 全槽比对（含根槽）。
- **证据**：`tests/audit_logs/night_review_probe.log` B 段、`review_round2.log` B 段（清屏后复测「目录已空」「df 行消失」均 PASS）。
- **结论**：✅ PASS。

## 6. P3 彩蛋与异常参数审查

| 探针 | 输入 | 结果 |
|---|---|---|
| cowsay 空消息 | `cowsay` | ✅ 默认 `Moo!` 气泡，不崩溃 |
| cowsay 消息 | `cowsay hello from review` | ✅ 正常气泡；消息截断 42 字符 |
| fortune | `fortune` | ✅ 随机引言，索引按条数取模有界 |
| yes 中断 | `yes` + Ctrl+C | ✅ 600 次上限内可中断，输出 `^C` |
| cal 当前月 | `cal` | ✅ 2026-11 日历正确 |
| cal 年视图 | `cal 2026` | ✅ 三栏 12 月无残留（#143） |
| cal 负数年份 | `cal -5` | ✅ `year out of range (1970..2099)` |
| cal 非数字 | `cal abc` | ✅ 同上拒绝 |

- **超长文本**：输入行上限 256，cowsay 消息入 `mbuf[44]` 截断 42，无栈溢出；yes 词指向输入缓冲，600 次有界。
- **证据**：截图 `A_cowsay.png`、`A_yes.png`、`A_cal_2026.png`；日志 A 段彩蛋组。
- **结论**：✅ PASS。

## 7. 五片合并命令面一致性（分发表 / help / TAB）

源码交叉核对（`shell.c`）：分发表 `strcmp_(cmd,...)`、help `help_row(...)`、补全表 `sh_cmds[]`、建议表 `BUILTIN_NAMES`。

| 片 | 命令 | 分发表 | help | TAB 补全 |
|---|---|---|---|---|
| P1 | run / ps / kill / sudo | ✅ | ✅ | ✅ |
| P2 | mount / umount / df | ✅ | ✅ | ✅（df #145 补入） |
| P3 | cowsay / sl / fortune / cal / yes | ✅ | ✅ | ✅（#145 补入） |
| P4 | pkg / head / tail / wc / tree | ✅ | ✅ | ✅ |
| 别名 | cls / su | ✅ | —（别名） | ✅（#145 顺带补入） |

- **实机**：help 中 pkg/head/tail/wc/tree/mount/umount/df/cal 在可见帧或回滚帧抓到；TAB 补全实测见 §2 #145。
- **取证限制**：控制台回滚查看器只绘制最近 `rows-1` 条存档行（#141 同类 UX 限制），help 顶部 cowsay/sl/fortune/yes 行滚出后真机无法回看；该四条以源码 `help_row` 调用点（shell.c:119-123）+ `kernel.bin` rodata 字符串（`strings -n 2`）交叉取证，命令本身在探针 A 段真机执行全部成功。
- **结论**：✅ PASS（命令面无合并丢失）。

## 8. P4 pkg 包管理器审查

**源码路径**：`shell.c cmd_pkg`（索引解析/安装/卸载/状态推导）、`builtin_apps.c` + `builtin_apps.inc`（三包种子 bc/stats/rps）、`tools/mkpkg.py`。

| 探针 | 结果 |
|---|---|
| `pkg list` 种子仓库 | ✅ bc/stats/rps 三条版本与描述 |
| `pkg info bc` | ✅ name/version/desc/file/status 齐全 |
| `pkg install bc` | ✅ 解包落 `/bin/bc.ce` |
| 列表标记已安装 | ✅ |
| 同名覆盖重装 | ✅ 二次 install 覆盖 `/bin` 同名 `.ce`，`bc 12 + 34 = 46` 运行正常 |
| `pkg remove bc` | ✅ 删除 `/bin` 副本，仓库副本保留 |
| remove 未安装包 | ✅ `bc is not installed` |
| install/remove 不存在包名 | ✅ `no such package: nosuchpkg` |

- **索引行损坏（源码核对）**：`pkg_load_index` 按 `|` 切四字段；空行/`#` 注释/字段不足四/首字段空均跳过，字段拷贝走 `pk_pad_copy` 定长截断，越界安全。
- **遗留潜在项 #147**：install 按 CPKG 头 `cmd` 字段落盘 `/bin/<cmd>.ce`，而 `pkg_installed/pkg_dst_path` 按索引 `name` 字段查 `/bin/<name>.ce`；当前播种三包 name==cmd 无差异，闭环真机 PASS。若日后发布 name≠cmd 的包，卸载将找不到已安装文件；建议后续让卸载侧读包内 cmd 头字段。
- **证据**：截图 `A_pkg_list.png`、`A_pkg_remove_absent.png`、`A_bc_run.png`；日志 A 段 pkg 组。
- **结论**：✅ PASS（#147 为建议项，非本轮缺陷）。

## 9. 工具迁移 tree/head/tail/wc 审查

| 探针 | 结果 |
|---|---|
| head/tail/wc 不存在文件 | ✅ `no such file`，三工具一致 |
| wc 空文件 | ✅ `0 0 0`（#144 修复后） |
| head/tail 空文件 | ✅ 无输出不崩溃 |
| wc 二进制 hello.ce | ✅ 7 行 / 33 词 / 4312 字节 |
| head 二进制 | ✅ 读到 `CE01` 头即止，不崩溃 |
| tree 目录 | ✅ 目录树列举，深度上限 6、名数上限 24 |

- **证据**：截图 `A_tools_binary.png`；日志 A 段工具组。
- **结论**：✅ PASS。

## 10. 回归基线复核

`tests/run_full_regress.sh`（2026-11-16 本轮重跑，日志 `tests/audit_logs/full_regress_v244.log`、分 driver 日志 `tests/regress_logs/`）：

| Driver | 退出 | 结论 |
|---|---|---|
| qemu | 0 | ✅ 全 PASS |
| fsrec | 0 | ✅ 全 PASS |
| caps | 0 | ✅ 全 PASS |
| user | 0 | ✅ 全 PASS |
| disk | 0 | ✅ 全 PASS |
| env | 0 | ✅ 全 PASS |
| heap | 0 | ✅ 全 PASS |
| size | 0 | ✅ 7/7 PASS |
| ce | 1（既有基线） | ⚠️ PASS=7 SKIP=13 **FAIL=11**，与基线逐条一致：allocprobe/bc/calc/curl/demo/gfx24/go/heaptest/mmaptest/netinfo/sndtest，均为 ce_sim 未建模 opcode（0x6a/0x08/ff /6/0x0a），真机应用正常 |

未打破既有基线；本轮未新增任何 ce_sim FAIL。

## 11. 遗留清单（B 可接受边界 / C 建议项）

| 编号 | 级别 | 事项 | 处置 |
|---|---|---|---|
| #141 | B | 超过 25 行的列举/help 顶部条目离开实时网格后，回滚查看器深度/窗口有限，真机不可回看 | 既有 deferred；本轮测试侧以源码 + rodata 交叉取证；后续可加分页暂停或加深回滚 |
| #147 | C | pkg 安装按 `cmd` 落盘、卸载按 `name` 查找，name≠cmd 时卸载失配 | 播种包对齐无影响；建议后续统一为读包内 cmd 头字段 |
| — | C | 部分既有命令（alias/chcaps/chown/env/gfx24/history/netcfg/setenv/unalias/useredit 等）无独立 help 行，多为别名或合并条目 | 非本轮合并回归，可后续补 help 行 |
| — | B | ce_sim 11 条 opcode 未建模 | 既有基线，不改 |

## 12. 探针与产物索引

- 探针脚本：
  - `tests/night_review_probe.py` —— 全量三引导探针（A PE/pkg/工具/彩蛋，B 挂载矩阵，C 3840 内存）
  - `tests/review_fix_verify.py` —— 4 个已知问题定点复现
  - `tests/review_round2.py` —— 清屏复测挂载矩阵、两档内存帧计数、hwinfo、别名补全
  - `tests/review_root_remount.py` —— 根分区二次绑定拒绝 + 重复挂载回归
  - `tests/review_help_scroll.py` —— help 深回滚取证
  - `tests/review_assets/gen_bad_pe.py` —— 非法 PE 样本生成器 + 样本
- 日志：`tests/audit_logs/night_review_probe.log`、`review_fix_verify.log`、`review_round2.log`、`review_root_remount`（随脚本输出）、`full_regress_v244.log`
- 截图：`tests/qemu_shots_review/`（V_* 修复验证、A_* 应用探针、B_* 挂载、C_* 内存）
- 问题清单：`docs/CatOS-内核问题清单.md` #142~#147
- 提交：v2.44 main 首条提交（hash 以 `git log --oneline` 为准）
