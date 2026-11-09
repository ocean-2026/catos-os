# CatOS 合并取精华 —— 评估与执行结论

> **文档定位**：本次「现项目 × 原项目合并取精华」第一阶段（仓库治理 + 资产收割 + 构建可复现性）
> 的**评估结论与执行记录**。
>
> **上游依据**：需求分析见 `docs/merge-harvest-PRD.md`（PM 产出，637 行，全文实测）。
> 本文档**不重复** PRD 的逐条核实过程，只回答四个问题：
> **做了什么** / **依据是什么** / **哪些结论被推翻了** / **下一步谁做什么**。
>
> **执行者**：软件工程师（Kou）｜ **隔离工作树**：`C:/Users/Admin/WorkBuddy/Worktrees/CatOs/master-87a3e1a0`
> （分支 `workbuddy/todo-20261106-1906`）｜ **执行日期**：2026-11-09
>
> **硬约束遵守情况**：`D:/CatOs` 与 `D:/CatOs/Catos_Better_better` **全程只读**
> （V-17：文件级 diff 零越界写入，仅读取与复制出来）；所有写入只发生在隔离工作树内；
> 未 push。

---

## 一、TL;DR

| 项 | 结论 |
|---|---|
| **E1 根目录治理** | ✅ 完成。删除 **18 个**零引用快照文件，迁移 5 份文档入 `docs/`，重写根 `README.md`。根目录从 11 项收敛到 4 项。 |
| **E2 资产归档** | ✅ 完成。归档 5 份原项目文档（逐份判定「是否仍然成立」），把 **11 条**现项目未记录的遗留问题落档进问题清单（表格行数 64 → 82）。 |
| **E3 构建可复现性** | ✅ 完成。方案 (a) 模板入库 + `.gitignore` 否定规则移位。**干净克隆实测出 ISO，12/12 QEMU 回归 PASS**。基线从此具备 CI 意义。 |
| **E4 node/go 移植可行性** | ✅ 结论采信 PRD §2.3/§2.5（PM 已实机跑通）。工程师侧**独立复核确认**，并**补充一条 PRD 未记录的重要发现**（见 §五）。 |
| **基线回归** | ✅ 三项全部保持，无回归（§六）。 |
| **推翻的既有结论** | 3 条（§五）。 |

---

## 二、E1 根目录治理

### 2.1 删除清单（18 个文件，删前逐个全仓 grep 复核）

| 文件 | 大小 | grep 证据 |
|---|---|---|
| `T1.C` `T2.C` `T3.C` `T4.C` `T5.C` `T6.C` | 各 1–37 B | 全仓 `git grep` **零命中**（编译器最小试验碎片） |
| `MIN.C` `MIN2.C` | 各 49–70 B | 全仓 `git grep` **零命中**（最小 CE 程序） |
| `cc_old.c` | 49,134 B | 零命中；且经 diff 判定为现项目 `cc.c` 的**祖先**（757 行唯一公共行），与原项目 `cc.c` 公共行 **0** ⇒ 确为陈旧快照 |
| `cc_old.ce` / `cc_old_ce.exe` | 各 26,952 B | 零命中（前者的编译产物） |
| `vga.c` | 4,992 B | 仅命中**文档叙述**（`docs/CatOS-渲染层前后端分离设计.md:82,161`）与旧根 `build.sh` 注释；功能已下沉到 `kernel_v25/src/vga.c`（27 行瘦包装 → `con_*`） |
| `gfx.c` / `gfx.h` | 15,531 / 1,158 B | 同上，仅文档命中。绘制原语在现项目**不存在** —— 这是 v2.7+ 架构**刻意**不暴露像素 syscall 的结果（`ce_abi.h` 注释："v2.7/v2.8 deliberately expose NO drawing API"），非遗漏 |
| `shell.c` | 49,095 B | 同上，仅文档命中；已并入 `kernel_v25/src/shell.c` |
| `shot-gfx-mode.png` / `shot-text-restored.png` | 4,171 / 1,120 B | 仅 `README.md:44-45` 与 `docs/CatOS-图形模式能力说明.md:170-171` 两处文档表格引用 |
| `Catos_Better_b` | 82,586 B | md5 `d21d66ea5845492dab406d04f666c60e` = 原项目 `build/CC2_NEW.CE` ⇒ 是 Stage-5 自举 cc 的 **CE01 二进制**，不是文档 |

### 2.2 保留与迁移

| 项 | 处置 | 理由 |
|---|---|---|
| 根 `build.sh` | **保留** | 不是残桩 —— 13 行可用委托 shim（`cd kernel_v25; exec sh build.sh "$@"`），实测从根执行能成功产出 iso；且 `docs/CatOS-渲染层前后端分离设计.md:82` 明确记载「`build.sh`（根）改成委托 `kernel_v25/build.sh` 的入口」。PRD §2.9/§4.1 亦判定「⚠️ 保留」。仅更新注释（不再称那些快照文件仍在仓库里） |
| `CatOS-图形模式能力说明.md` | → `docs/` | git mv，内容保留 + 加归档说明 |
| `CatOS-渲染层前后端分离设计.md` | → `docs/` | git mv，更新两处对已删快照文件的引用 |
| `CatOS-进度总结与待办-2026-11-02.md` | → `docs/` | git mv |
| `CatOS_用户反馈分析.xlsx` | → `docs/` | git mv（需求输入，非代码垃圾） |
| 旧根 `README.md` | → `docs/CatOS-v0.3-图形模式成就包.md` | 它是 **v0.3 成就包说明**（Mode 13h + QEMU VGA 字形 bug 根因分析），**不是项目 README**；内容作为历史记录保留 |
| **新**根 `README.md` | 新建 | 归档后仓库顶层无 README 不合适。新 README 描述目录结构 / 构建 / 运行 / 测试 / 文档索引 |

### 2.3 根目录最终形态

```
.gitignore   README.md   build.sh   docs/   kernel_v25/   tests/
```
（`git ls-files | grep -v /` 只返回 `.gitignore` / `README.md` / `build.sh` 三项；
`.workbuddy/` 已被 `.gitignore:64` 忽略。）

---

## 三、E2 资产归档

### 3.1 归档清单与判定

完整索引见 **`docs/orig-archive/README.md`**（5 份逐份判定 + 依据 + 关键文件位置）。
摘要：

| 归档后路径 | 原路径 | 是否仍然成立 |
|---|---|---|
| `docs/GO-DESIGN.md` | `doc/GO-DESIGN.md` | **部分成立** —— 设计/子集规格/自举定义有效；§2「48 槽」过期（现 57 槽，但 go.c 只用 ≤39 槽 ⇒ 无需改 ABI）；§6「未修 FIND-G3~G10」过期 |
| `docs/go-review.md` | `doc/go-review.md` | **方法与 FIND 清单有效，缺陷状态过期** |
| `docs/orig-archive/legacy-TODO_STAGE5.md` | `doc/TODO_STAGE5.md` | **部分成立** —— 自举固定点、128 KiB 栈仍未做 |
| `docs/orig-archive/legacy-BUILD_REPORT_STAGE5.md` | `doc/BUILD_REPORT_STAGE5.md` | **部分成立** —— 工具链根因已解决；3 项待验证仍在 |
| `docs/orig-archive/legacy-HANDOFF_STAGE5.md` | `doc/HANDOFF_STAGE5.md` | **部分成立** —— 128 KiB 栈改动仍在；**forward-call-fix 是否并入现项目待架构师核对** |

未归档的 9 份（原项目 `doc/` 6 份 + 根 3 份）逐条给了理由，见 `docs/orig-archive/README.md` 第一节。

### 3.2 原项目 `tools/` 三脚本：全部实际试跑，全部不移植

| 脚本 | 试跑结果 | 判定 |
|---|---|---|
| `cc_stage45_test.py` | `subprocess.run(['pkill','-9','-x','qemu-system-i38'])` → `FileNotFoundError`（Linux-only）+ 硬编码 `/home/user/Doubao/chats/38448438262014978/catos-qemu` | 不移植（功能被 `tests/qemu_regress.py` 覆盖） |
| `diag2_test.py` | 同上 | 不移植（同上） |
| `inject_any.py` | 与现项目 `tests/inject_any.py` **逐字节相同**；几何参数 `RSVD=33`/`SPC=8`/`FAT_SZ=8192` vs 内核 `src/fat.c:8` `RESERVED 32u` ⇒ 在现项目上必然 `BIN dir not found` | 不移植原文件，**改为修现项目副本**（落档 `#64`） |

`tools/TNC2.C`（1 行，DREL 数据区重定位回归用例）归入移植清单（PRD P1-6），本阶段不重复搬运。

### 3.3 D-1~D-15 落档

`docs/CatOS-内核问题清单.md` 表格行数 **64 → 82（+18）**：

| 位置 | 编号 | 内容 | PRD 关联 |
|---|---|---|---|
| 第一节 | `#48` | `heap_ensure_range()` 缺失 ⇒ 跨页分配 `#PF` | P0-2 |
| 第一节 | `#49` | `USER_STACK_SZ` 64 KiB（应 128 KiB） | P0-3 |
| 第一节 | `#50` | `USER_HEAP_MAX` 256 KiB（应 512 KiB） | P0-3 |
| 第四节 | `#51` | `cc.c` 停在阶段 0，`cc_self.c` 是死代码 | P1-3 |
| 第四节 | `#52` | `CC2.CE` 自举固定点未验证 | P2-1 |
| 第四节 | `#53`~`#60` | Go 解释器 FIND-G3~G10（8 条各占一行） | P1-2 |
| 第四节 | `#61` | `catos_work.iso` 从未入库 | P0-4 |
| 第四节 | `#62` | `build.sh` 应用列表不含 `cc` | P1-4 |
| 第四节 | `#63` | `node.c` 从不被编译 | P1-9/P1-10 |
| 第四节 | `#64` | `tests/inject_any.py` FAT 几何过时 | P1-7 |
| 第四节 | `#65` | `.gitignore` 否定规则位置写反 | P0-4 |

D-4 / D-5 / D-9 / D-10 按 PRD 判定**不新增**（现项目已分别记录于 `#17`、第十节 `#47`、第十节节末表）。

**编号冲突消解**：第九节 `#48`/`F1`/`A2`/`#3` 与第十节 `#47` 本就是**各自独立的局部命名空间**
（第九节是 b31 修复闭合表、第十节是工具链常量核对表，均已在节首自述）。
本次在**第一节**与**第九节**各加了一段命名空间说明，明确「引用时写『第四节 #61』」。

---

## 四、E3 构建可复现性

### 4.1 方案选择

| 路线 | 结论 |
|---|---|
| (b) 现场用 `grub-mkrescue` 生成模板 | ❌ **否决**。`grub-mkrescue` / `grub2-mkrescue` / `grub-mkimage` / `xorriso` / `mformat` / `mcopy` / `genisoimage` / `mkisofs` **全部 NOT FOUND**（无 MSYS2、无 `C:\grub*`、无 `D:\grub*`、无 choco shim）。写不出能在本机真跑起来的 bootstrap 脚本 |
| (a) 模板入库 | ✅ **采用**。一次性 10.7 MB，换任何人 clone 下来都能构建 |

### 4.2 根因：不是「忘了 git add」，是 `.gitignore` 否定规则写反

git 解析 `.gitignore` 是 **last-match-wins**。原文件第 7 行与第 12 行都有
`!kernel_v25/build/catos_work.iso`，而第 27 行是 `*.iso` —— 后者把前者**覆盖**了。
原注释写的 *"exceptions must precede rules"* **结论是错的**。

```
修复前：git check-ignore -v kernel_v25/build/catos_work.iso
        → .gitignore:27:*.iso          ← 命中的是【忽略规则】，否定规则形同虚设
修复后：git check-ignore -v --no-index kernel_v25/build/catos_work.iso
        → .gitignore:35:!kernel_v25/build/catos_work.iso   ← 命中的是【否定规则】
```

已把两条否定规则移到 `*.iso` / `*.py` **之后**并重写注释（明确写出 last-match-wins 语义）。
**只 `git add -f` 而不移位是假修复** —— 下次有人删了重建该文件会再次被 `*.iso` 忽略。

### 4.3 验收：干净克隆实测

`git clone` 本分支到 `/tmp/catos_clone_test` 后：

```
git ls-files kernel_v25/build/ → kernel_v25/build/catos_work.iso
                                  kernel_v25/build/gen_builtin.py
                                  kernel_v25/build/mkce.py
                                  kernel_v25/build/patch_work_iso.py
build.sh iso → OK -> build/kernel.bin
              OK -> ...\kernel_v25\catos.iso (16558080 bytes)
tests/test_mkce.py   → 48 passed, 0 failed
tests/ce_regress.py  → PASS=5 SKIP=13 FAIL=0
tests/qemu_regress.py→ steps=12 ok=12 warn=0
```

**意义**：在 E3 之前，"基线 12/12 PASS" 依赖 team-lead 手工拷进工作树的**未跟踪**模板，
**不具备 CI 意义**；E3 之后基线可由任何第三方在干净克隆上复现。

---

## 五、E4 node/go 移植可行性 —— 结论与独立复核

> **任务取消**：PM 已在 PRD §2.3 / §2.5 完成 spike 并**亲自在 QEMU 实机跑通**，
> 本节只做**工程师侧独立复核**并补充一条 PRD 未记录的重要发现。

### 5.1 结论（采信 PRD）

| 资产 | 结论 | 依据 |
|---|---|---|
| `go.c` | **可移植，ABI 零改动** | 只用 7 个槽（`write`0/`putc`1/`alloc`11/`print_int`14/`atoi`18/`file_exists`23/`file_read`39），**全部 ≤39 < 48**，落在 57 槽 append-only 旧前缀内。PM 实测：57 槽头编译零错、CE01 头合法、QEMU 输出 `fib(10)= 55` / `demo done` / `go: exit ok` |
| `node.c` | **不需要移植 —— 已经在库里** | `kernel_v25/node/node.c`（1,414 行）与原项目 `node/node.c` diff **0 行差异**；`node.ce` **逐字节相同**（md5 `2101a0c8afe0243d192fd86f27350a94`）；`TEST.JS` 仅 CRLF 差异。**真实缺口是它从不被任何构建脚本编译**（落档 `#63`） |
| 原项目 `src/` | **不是"另一套实现"，是现项目的祖先** | 19 个文件逐一对过，10 个零行差异、7 个现项目更新。"整树合并 src/"是伪命题 —— 精华在 `go.c` + 文档 + 构建链缺口 + 两个被 VMA 重构弄丢的内核修复 |

### 5.2 独立复核发现（PRD 未记录）

**PRD §2.7 与 `go-review.md` 都把 FIND-G3~G10 记为「未修」，但原项目 `extapp/go.c` 源码里这 8 条其实已经全部修好。**

| FIND | 修复证据（`extapp/go.c`，1,300 行 / md5 `f9876f42fa646c58ec21da649ed934e4`） |
|---|---|
| G3 递归深度 | `:783` `g_call_depth` + `:784` `CALL_DEPTH_MAX 200` + `:1048` 入口检查 + `:1053/:1058/:1074` 增减配平 |
| G4 非法字符静默 | `:358-362` default 分支置 `g_perr`/`g_errline` |
| G5 readfile 16 KiB 上限 | `:1085` 新增 `read_whole()`，按 `FS_CHUNK 16384` 分块循环读至 EOF |
| G6 arena 未判空 | `:701` 与 `:732` `if (!s->v) { g_oom = 1; return 0; }` |
| G7 字面量 1023 截断 | `:223-235` 两遍扫描（先数解码长度再按 arena 尺寸分配）；未闭合引号报词法错误 |
| G8 顶层 break/continue | `:188-191` `g_loop_depth`/`g_func_depth` + `:658/:665/:669` 三处 `depth==0` 报错 |
| G9 for `:=` 作用域泄漏 | `:882-899` 每个 for 建独立 `Scope`，结束恢复 `g_scope` |
| G10 数字溢出 | `:302-313` 扫描时检测 `> 2147483647` |

**含义**：用户拍板的「8 个 P2 全部修」，**在源码层面原项目已经完成**。
移植时要做的是**逐条验证修复随行**，而不是重新实现 —— 这直接改变了 P1-2 的工作量估算。

### 5.3 对现项目 `cc.c` 的边际价值（诚实评估）

**结论：把 `node.c` / `go.c` 拉回来的边际价值，不在于「多一个程序」，而在于「多一条独立于内核的验证路径」与「补上现项目缺失的一整条工具链能力」。**

| 维度 | 评估 |
|---|---|
| **对 `cc.c` 自举路线的直接价值** | **低**。`node.c` 是 JS 解释器、`go.c` 是 Go 解释器，都不是 C 编译器，与 `cc.c` 的自举**无直接依赖**。`cc.c` 的能力缺口（`struct`/`typedef`/`enum`/`switch`/`goto`）只能由 `cc_self.c`（现项目内，2,250 行，已实现）补齐 —— 这才是 `cc.c` 的正确解法 |
| **对「编译能力已废」问题的价值** | **间接但真实**。`cc.c` 编译能力实际已废（P0-1：`api->alloc()` 恒返回 NULL ⇒ 只能编译 <8 KiB 源码）。`go.c` 的 192 KiB arena 与 `cc_self.c` 的编译需求**都依赖同一个堆修复** ⇒ 它们是**同一个 P0 的多个受害者**，不是解决方案 |
| **对内核回归测试的价值** | **中**。`go.c`/`node.c` 是**用户态真实负载**：能跑它们 ⇒ 内核的 alloc/free/fs/console 路径被真实压测；跑不了 ⇒ 可能只是 alloc 坏了。PRD V-5/V-6 的 allocprobe/heaptest 探针是**更直接**的验证手段，但 `go.ce` 是**端到端**的验证 |
| **对文档与知识资产的价值** | **高**。`GO-DESIGN.md` + `go-review.md` 是**唯一**记录「为什么这样设计」的材料；`docs/` 里现项目对 `go.c` 的提及次数原本是 **0** ⇒ 这是一条被现项目主线完全遗忘的开发线 |
| **对 CE ABI 的验证价值** | **中**。`go.c`/`node.c` 都只用 ≤39 槽，是「append-only 策略确实保护了旧程序」的一个**活证据**。这比任何文档声明都强 |

**一句话**：边际价值**真实但不在编译能力上**。`go.c` 的价值是「用户态解释器 + 端到端验证负载 + 设计上下文」；
`node.c` 的价值是「补上构建链的一环 + 一条独立验证路径」。
如果只从「让 `cc.c` 能自举」这个目标看，**拉回 node/go 的价值接近于零** —— 那件事只需要 `cc_self.c` + 堆修复。

---

## 六、基线回归结果（真实输出）

| # | 命令 | 期望 | 实测 |
|---|---|---|---|
| A-1 | `python tests/test_mkce.py` | 48 passed / 0 failed | ✅ **48 passed, 0 failed** |
| A-2 | `python tests/ce_regress.py` | PASS=5 SKIP=13 FAIL=0 | ✅ **PASS=5 SKIP=13 FAIL=0** |
| A-3 | `python tests/test_cc.py` | PRD 写 8/8（该行标「待复测」） | ✅ **10 passed, 0 failed** —— 实测是 **10/10**，比 PRD 预期多 2 项（PRD 该行本就未复测，是沿用旧计数） |
| A-4 | `python tests/qemu_regress.py kernel_v25/catos.iso --qemu "C:/qemu-portable/qemu-system-i386.exe"` | steps=12 ok=12 warn=0 | ✅ **steps=12 ok=12 warn=0** |
| — | `kernel_v25/build.sh iso` | 产出 ISO | ✅ **OK -> catos.iso (16558080 bytes)** |

> **A-4 的 QEMU 路径更正**：任务书给的 `E:\Tools\qemu\qemu-system-i386.exe` 在本机**不存在**
> （`E:\Tools\qemu` 下只有 `qemu-edid/ga/nbd/storage-daemon` + `lib/`）；
> 实际可用路径是 **`C:\qemu-portable\qemu-system-i386.exe`**（QEMU 11.1.0）。
>
> **A-4 逐步输出（主工作树，2026-11-09）**：
> `boot-shell` / `format` / `help` / `display` / `gfx` / `text-reject-N` /
> `text-accept-Y` / `ls-bin` / `mmaptest` / `hello-elf` / `vdiag` /
> `final-frame` —— 12 步全 PASS，`warn=0`。
>
> **干净克隆复测**（`/tmp/catos_clone_test`，E3 后新增的一次验证）：
> `48 passed, 0 failed` / `PASS=5 SKIP=13 FAIL=0` / `steps=12 ok=12 warn=0`
> —— 与主工作树一致，证明基线不依赖任何本地状态。

---

## 七、被推翻的既有结论（3 条）

| # | 原结论 | 出处 | 实际情况 |
|---|---|---|---|
| 1 | 「`node.c` 是待拉回的候选资产」 | 任务书 E4 | **不成立** —— `node.c`/`node.ce`/`TEST.JS` **早已在库**且与原项目等价（`node.ce` 逐字节相同）。真实缺口是**它从不被编译**（`#63`），不是「缺文件」 |
| 2 | 「FIND-G3~G10 全部未修，需实现方逐条修」 | PRD §2.7 / `go-review.md` | **已过期** —— 原项目 `go.c` 源码里这 8 条**已经修好**（§5.2 逐条证据）。工作内容从「实现」变成「验证随行」 |
| 3 | 「`build.sh`（根）是残桩，应删除」 | 任务书 E1 | **不成立** —— 它是**可用**的委托 shim（实测能出 iso），且被 `docs/CatOS-渲染层前后端分离设计.md:82` 明确记载为设计的一部分。已保留并只更新注释 |

> 另有一条**元结论**：E3 的根因不是「忘了 `git add`」，而是 **`.gitignore` 的否定规则位置写反**
> （原注释 "exceptions must precede rules" 方向说反了）。只 `git add -f` 不移位是**假修复**。

---

## 八、提交与交付

| commit | 内容 |
|---|---|
| `66bbfce` | `curation: purge snapshot junk from repo root` —— E1 |
| `101a326` | `harvest: archive legacy docs + record D-1..D-15 in issue tracker` —— E2 |
| `f39bb0b` | `build: make ISO build reproducible from a clean clone` —— E3 |

**未 push**（按任务书要求）。他人 WIP（`kernel_v25/extapp/bin/*.ce` 13 个 + `kernel_v25/src/builtin_apps.inc`
的未暂存改动，源于本地 gcc Rev5 重编、committed 版内嵌 "Rev4"）**未纳入任何 commit**。

### 本阶段**未做**的事（诚实清单）

| 未做项 | 原因 |
|---|---|
| E4 的 go.c / node.c **实际移植** | 任务取消（PM 已完成 spike）；且 `gen_builtin.py` 的 `FILES` 列表、`extapp/go.c`、`extapp/cc.c` 明确划归架构师与后续实施阶段，不在本阶段范围 |
| P0-1 / P0-2 / P0-3 的**代码修复** | 属架构师设计与后续实施阶段；本阶段只负责**记录**（`#48`~`#50`），未动 `task.c` / `paging.h` |
| 原项目 `build/` 下 7 个宿主工具链资产（`host_stub_cc.c`/`host_stub_go.c`/`verify_bootstrap.sh`/`mk_cc_self.py`/`cc_qemu_test.py`/`go_qemu_test.py`/`cc_stage_test.py`）的归档 | PRD 把它们归为 P1-8 / P2-2 实施项；本阶段仅在归档索引中记录其存在与价值 |
| 原项目 `extapp/cases/` 11 个回归用例的移植 | PRD P1-5，属实施阶段 |
| 根 `README.md` 是否保留的最终裁决 | 已按「归档旧 README + 新建项目 README」处理，若 team-lead 认为顶层不该有 README，可撤掉新建的那个文件 |

---

## 九、下一步交接

| 责任方 | 事项 | 依据 |
|---|---|---|
| **架构师** | `docs/merge-harvest-design.md`：P0-1（`heap_vma_init` 用 `vma_insert` 而非赋值覆盖）、P0-2（`heap_ensure_range` 前置声明位置）、P0-3（栈/堆常量） | PRD §2.4、本文档 §三 |
| **架构师（核对）** | forward-call-fix（原项目 `extapp/cc.c:1832` 的原型符号复用逻辑）是否已并入现项目 `cc_self.c` | `docs/orig-archive/README.md` A-5 判定表 |
| **实施阶段** | P1-1（`go.c` 进内核 + 移植 5 个 `.GO` 用例，**逐条验证 FIND-G3~G10 修复随行**）、P1-3（`cc_self.c` 覆盖 `cc.c`）、P1-4/P1-9（补构建链）、P1-7（修 `inject_any.py`） | PRD §3、本文档 §五 |
| **工程师（后续）** | `docs/merge-harvest-design.md` 落地后，实施阶段收尾时的回归验证 | — |
