# 离线重定位动态链接设计（路线 1 落地详细设计）

> 基线：HEAD `6cf1ff6`（v2.45）。承接 `docs/pe-dynamic-linking-design.md` 路线 1（离线/装载期静态重定位：`mkpe.py` 离线 `.reloc` 重定位 + 外部能力绑定 vsyscall 表）。
> 状态：**设计文档**，不含已落地代码。配套差距分析见 `docs/os-gap-analysis-2026-11-17.md` §2.2。
> 范围约束：本文只设计路线 1；路线 2（固定基址共享模块）、路线 3（完整运行时动态链接）仍 deferred。

## 1. 目标与非目标

### 1.1 目标

1. 让非首选基址加载的 32 位 PE32 EXE 能在 CatOS 用户竞技区内正确运行：解析 `.reloc` 基址重定位块，把镜像内绝对引用按实际加载基址修正。
2. 外部能力调用在**构建/翻译期**静态绑定到 CatOS vsyscall 表槽位（`VS_TABLE = 0x2080800`，append-only ABI），产出自包含的 CE01/ELF 镜像。
3. 保持现有 PE 装载路径闭环：无 `.reloc`、无导入表的旧 PE 行为完全不变；不可重定位/不支持的镜像返回明确错误码，不崩溃。
4. 改造以离线工具侧为主、内核侧最小改动为原则（与 pe-design §3 路线 1「内核改动量小」结论一致）。

### 1.2 非目标（明确 deferred）

- 运行时动态装载新模块、DLL 依赖闭包装载、懒绑定 GOT/PLT、模块卸载与引用计数（路线 3 前置：跨任务共享只读页、符号表查询接口、模块生命周期，均不具备）。
- 跨任务共享同一份库代码（路线 2 固定基址库区）；本路线每个镜像各拷一份库代码，空间换复杂度。
- 导出目录解析、按名称/序号解析外部符号的运行时查表。
- ARM64 / 64 位 PE / 高地址 ImageBase（≥0x80000000）支持；DLL 不作为主目标（沿用 mkpe.py 现有 EXE 约束）。

## 2. 现状基线（源码定位）

- PE 装载器：`src/task.c:2086` `spawn_load_pe()`——解析 DOS header → PE sig → COFF → Optional header → section headers → entry；收集合并段页范围（2134 行附近）；**导入表 / 导出表 / `.reloc` 一律忽略**（问题清单 #130）。
- 地址区：用户竞技场 `USER_IMAGE_BASE 0x2000000` ~ `USER_EXT_END 0x2800000`；ELF PIE 走扩展竞技场 + ASLR；PE 轨道要求镜像落在竞技场首选基址附近。
- 离线翻译：`tests/mkpe.py`——`parse_pe()`（149 行）解析头/段表并拒绝高 ImageBase（181 行）；`pe_to_ce()`（206 行）按段排序拼扁平镜像；约束：EXE 非 DLL、入口落在 flat 镜像偏移 0（#131）。
- vsyscall：`USER_VSYSCALL = 0x2080000`，函数指针表 `VS_TABLE = 0x2080800`；CE 镜像 `api_offset` 处 4 字节槽由装载器写入表指针；槽位 append-only（现 65+）。
- 权限闸口：`task.c:2369–2398` PERM_EXEC 统一闸口（CE/ELF/PE 三轨共用，拒绝返回 -13 EACCES）。
- 畸形用例基建：`tests/review_assets/gen_bad_pe.py`；已有样本 `BADA_LF.EXE / BADMZ.EXE / OVERSEC.EXE / PESAMPLE.EXE / TRUNC_PE.EXE`。

## 3. `.reloc` 段格式（实现依据）

### 3.1 目录与块结构

- 重定位信息由 Optional Header 数据目录 **[5]（IMAGE_DIRECTORY_ENTRY_BASERELOC）** 定位：RVA + 大小。目录为空（size=0）表示镜像已链接到首选基址，无需重定位。
- 重定位由若干连续块（IMAGE_BASE_RELOCATION）组成，每块 8 字节头：
  - `VirtualAddress`（4 字节）：本块覆盖的页 RVA；
  - `SizeOfBlock`（4 字节）：块总字节（含头）；
  - 其后为 `(SizeOfBlock - 8) / 2` 个 16 位条目。
- 条目 16 位：高 4 位类型 `Type`，低 12 位 `Offset`（页内偏移）；条目指向的 32 位位置 RVA = `VirtualAddress + Offset`。

### 3.2 条目类型（本路线只支持其一）

| Type | 名称 | 处理 |
|---|---|---|
| 0 | IMAGE_REL_BASED_ABSOLUTE | 填充对齐，跳过 |
| 3 | IMAGE_REL_BASED_HIGHLOW | **唯一支持**：32 位绝对地址，补丁值 = 原值 − PreferredImageBase + ActualBase |
| 其余（HIGH/LOW/HIGHADJ/SECTION/SECREL/ARM 等） | — | 遇到即标记镜像不可重定位，返回明确错误码 |

### 3.3 修正公式（32 位 x86）

```
delta = ActualLoadBase − PreferredImageBase
for each HIGHLOW entry:
    addr_rva = block.VirtualAddress + entry.Offset
    if addr_rva 落在某已映射段范围内:
        *(uint32_t *)(image + addr_rva) += delta
    else:
        畸形/越界 → 拒绝装载（见 §7）
```

- `PreferredImageBase` 取自 Optional Header ImageBase；`ActualLoadBase` 为本次装载实际基址。
- 若 `ActualLoadBase == PreferredImageBase`，delta=0，可跳过补丁（仅校验条目落在段内）。

## 4. 离线生成流程（`mkpe.py` 改造点，主路径）

> 推荐主路径：**离线完成全部重定位**，产出固定基址 CE01 flat 镜像；内核不承担补丁计算。内核侧补丁循环作为 §5 的兜底，仅用于装载期基址与首选基址不同的 ELF/PE 混合场景。

### 4.1 改造步骤

1. **解析数据目录 [5]**：`parse_pe()` 增加 reloc 目录 RVA/大小字段；解析失败或目录越界按畸形 PE 处理（§7）。
2. **块/条目解析**：遍历块链，抽取 `(Type, RVA)` 列表；ABSOLUTE 跳过；HIGHLOW 保留；其他类型记入 `unsupported_reloc_types`。
3. **段区间校验**：每个条目 RVA 必须落在某段 `[VirtualAddress, VirtualAddress+VirtualSize)` 内；越界即报错，不产出镜像。
4. **离线基址选定与补丁**：
   - 目标 CE01 flat 镜像固定基址（沿用现有 CE 装载约定）；按该基址计算 delta，在翻译后的扁平镜像缓冲内逐条 += delta；
   - 补丁只允许落在可写/数据段与代码段内的绝对地址槽；只读 `.text` 内的 HIGHLOW 条目按 PE 语义同样修正（装载时 `.text` 暂在可写拷贝区，映射后只读）。
5. **导入绑定（vsyscall 静态绑定）**：
   - 解析导入目录 [1]：DLL 名 + ILT/IFT；建立「外部符号名 → CatOS vsyscall 槽位号」映射表（离线维护一张 PE 导出名 ↔ VS_TABLE 槽位对照清单，清单本身 append-only 扩展）；
   - 为每个被引用外部函数生成一个离线桩节（thunk）：跳转到对应 vsyscall 槽位间接调用；镜像内调用点/ IFT 槽改写为桩地址；
   - 未映射到任何 CatOS 槽位的外部符号：翻译失败，输出明确缺失符号清单（不产出自称可用的镜像）。
6. **产物与元数据**：CE01 头保持 20 字节现有布局；若内核需兜底校验，可在扁平镜像尾部追加一张**重定位补丁表**（RVA 列表，不含原值——delta 由装载时基址差推导），格式在 §5.3 定义；无补丁表的旧镜像按现状处理。

### 4.2 `mkpe.py` 新增错误码（PEError 子类/文案）

- `reloc directory out of file`；`reloc block truncated`；`reloc entry out of section`；`unsupported reloc type 0xN`；`unresolved import: <name>`；`high ImageBase unsupported`（沿用现有）。

## 5. 加载期基址重定位（内核侧改动点，兜底路径）

### 5.1 触发条件

`spawn_load_pe()`（`task.c:2086`）在段拷贝完成、镜像定位到实际基址后：
1. 读取翻译产物携带的补丁表（§5.3）；无表 → 走现状（不打补丁，#130 既有行为）。
2. 计算 `delta = ActualLoadBase − PreferredImageBase`；delta=0 仅做区间校验。
3. 逐条应用 HIGHLOW 修正。

### 5.2 内核补丁循环要点

- 补丁在**任务私有可写拷贝区**完成，映射只读权限在补丁之后生效；与现有段收集/合并流程（2134 行附近）衔接。
- 每条目先做段区间边界检查（RVA ∈ 某段内存范围、32 位对齐可读可写），越界即放弃装载并返回错误码，禁止裸解引用（复用 #78/#86 `copy_from_user` 同类边界纪律）。
- 重定位表长度设上限（条目数/字节数双阈值），防止畸形镜像在内核态做大循环；超限拒绝。
- 补丁循环不申请新内存、不引入锁；保持非抢占假设（与 #86 static 缓冲安全模型一致）。

### 5.3 补丁表格式（离线产物尾部，小端）

```
offset 0:  u32 magic 'CPR1'
offset 4:  u32 preferred_image_base
offset 8:  u32 entry_count
offset 12: u32 reserved (0)
followed:  entry_count × u32 rva   (HIGHLOW 条目 RVA；类型已在离线侧归一为 HIGHLOW)
```

- 内核只认 HIGHLOW；离线侧已剔除 ABSOLUTE 与不支持类型。
- 表紧跟扁平镜像数据，长度记入 CE01 扩展元数据；镜像尺寸上限沿用 `PE_MAX_FILE`，不抬高。

### 5.4 与现有 PERM_EXEC 闸口的交互

- 重定位/导入绑定全部发生在 **PERM_EXEC 闸口放行之后**（闸口 `task.c:2369–2398` 先于装载）：无执行权限的镜像不会进入段映射与补丁流程，避免对无权访问文件做内核态解析。
- 闸口错误码保持 -13 EACCES；重定位/格式错误使用独立错误码（沿用现有 -5 坏镜像族，细分 -16  reloc 越界、-17 未解析导入，具体号段在实现时与现有 CE 错误码表对齐，不冲突）。
- root bypass 与 owner/group/other 语义不变。

## 6. 回滚与兼容策略

1. **无 `.reloc` / 无补丁表的旧 PE**：装载路径与现状逐字节一致；内核补丁循环检测到无表即跳过，行为不变（#130 既有约束文档化为返回「需离线转换」提示的场景）。
2. **离线翻译失败不污染原镜像**：`mkpe.py` 任何一步校验失败即非零退出，不写出 `.ce`；磁盘上旧产物保留。
3. **ABI append-only**：vsyscall 槽位只在表尾追加；新增映射所需槽位走现有 CE ABI 追加流程，老二进制不受影响。
4. **灰度开关**：内核侧补丁循环可加编译期/运行期开关（默认开）；出问题时回退到「无补丁表即不补丁」的现状路径，无需回滚工具链。
5. **旧样本回归**：`tests/review_assets/` 既有 PE 样本必须在改造后全部通过原有用例；新增用例不改变旧样本期望输出。
6. **文档回滚**：本文档与 pe-dynamic-linking-design.md 路线 1 段落对应；若路线 1 落地受阻，退回 P1 静态子集约束（只支持基址落在竞技场、无外部导入的 PE）。

## 7. 测试方案

### 7.1 正常路径

- 首选基址 = 实际基址：delta=0，补丁表存在但不改动数值；镜像 entry 正常跳转。
- 非首选基址：手工构造含 HIGHLOW 绝对引用（全局指针/跳转表）的样例 PE，离线补丁后上屏/调用结果与宿主预期一致。
- 导入绑定：样例调用 2~3 个已映射 vsyscall 槽位（如 write/delay/print_int），输出与原生 CE 应用一致。
- 往返：mkpe.py 翻译 → 内核装载 → 运行；`ps`/退出码/wait 返回值正常。

### 7.2 恶意 / 畸形 PE 用例（扩展 `gen_bad_pe.py`）

| 用例 | 构造 | 期望 |
|---|---|---|
| 截断的 reloc 目录 | 目录 RVA/大小指向文件尾之外 | 拒绝装载，明确错误码，不 panic |
| 块头截断 | 块大小超过剩余文件 | 拒绝 |
| 条目 RVA 越界段 | 条目 RVA 落在所有段之外 | 拒绝（-16 族） |
| 不支持的重定位类型 | Type=4/5/10 等 | 拒绝并报 unsupported type |
| 超大条目数 | 条目数超内核阈值 | 拒绝，不进入长循环 |
| 重复/重叠块 | 同页两块重叠 | 离线侧告警拒绝；内核侧去重或拒绝 |
| 未解析导入 | 引用无对照清单的 DLL/函数 | 翻译失败，列出缺失符号 |
| 既有坏样本 | BADMZ / BADA_LF / OVERSEC / TRUNC_PE | 保持现有拒绝行为不回归 |
| 镜像尺寸超限 | 超 `PE_MAX_FILE` | 沿用现有 -3/-5 拒绝 |

### 7.3 回归

- 现有 PE/ELF/CE 三轨装载用例全绿；管道/后台/权限闸口（PERM_EXEC -13）用例不回归。
- 离线工具单测：块/条目解析、delta 计算、RVA 校验、桩节生成，覆盖 §7.2 各类畸形输入。
- 真机/QEMU 冒烟：翻译产物启动、调用 vsyscall、退出回收（ZOMBIE 被 reaper 回收）。

## 8. 风险清单

| 风险 | 等级 | 缓解 |
|---|---|---|
| 补丁循环在内核态越界解引用 → 停机 | 高 | 条目区间校验前置；边界检查复用 #78/#86 纪律；阈值限长；畸形用例全覆盖 |
| 导入映射清单不全，外部能力绑不上 | 中 | 离线侧拒绝并列出缺失符号；清单 append-only 逐步补槽 |
| 离线补丁与内核兜底补丁两处逻辑漂移 | 中 | 公式/类型归一在离线侧完成；内核兜底只做 HIGHLOW += delta，逻辑保持一页可读 |
| CE01 头/尾部元数据与现有装载器解析冲突 | 中 | 元数据尾部追加、 magic 校验；无表旧镜像走原路径 |
| 绝对引用存在于未覆盖段（如栈上构造的地址） | 中 | 文档明示限制：不处理运行期计算地址；此类镜像归路线 3 |
| 非抢占假设下补丁循环过长影响调度时延 | 低 | 条目数阈值；补丁在装载一次性完成，不在 syscall 热路径 |
| 与并行批次 B 代码改动冲突 | 低 | 本文档只新增 docs 文件；实现期再按当时 task.c 现状核对行号 |

## 9. 工作量估计

> 粒度：人日（pd，按 1 人 1 天有效工时 6 h 计）；数字为规划估计，实现时按实际编译/回归校准。

| 阶段 | 内容 | 估计 |
|---|---|---|
| D1 | mkpe.py：reloc 目录/块/条目解析 + 段区间校验 + 畸形错误码 | 4–6 h（0.7 pd） |
| D2 | mkpe.py：离线基址选定 + HIGHLOW 补丁 + 扁平镜像产物 | 3–5 h（0.6 pd） |
| D3 | mkpe.py：导入目录解析 + 外部符号↔vsyscall 槽位对照清单 + 桩节生成 | 6–8 h（1.1 pd） |
| D4 | 内核：补丁表识别 + 边界检查补丁循环 + 错误码族 | 4–6 h（0.8 pd） |
| D5 | 测试：gen_bad_pe.py 扩充畸形用例 + 离线单测 + 三轨回归 | 5–7 h（1.0 pd） |
| D6 | QEMU 冒烟真机验证 + 文档/错误码对齐 + 收尾 | 3–4 h（0.6 pd） |
| **合计** | 路线 1 落地（不含路线 2/3） | **约 28–36 h ≈ 4.7–6.0 pd** |

前置依赖：无阻塞性内核重构；可在当前扁平地址空间模型内完成。路线 2/3 所需共享页、符号表、模块生命周期不在本估计内。

## 10. 与其他文档的关系

- `docs/pe-dynamic-linking-design.md` §3 路线 1：本文是其落地展开；§4 内核缺口清单第 4 项（重定位引擎）在本文 §5 落地。
- `docs/os-gap-analysis-2026-11-17.md` §2.2：差距评级「中」，今夜可行性标注「路线 1 离线重定位可增量；运行时动态链接 deferred」。
- 问题清单 #130（PE 未实现基址重定位）、#131（入口偏移 0 约束）：本文落地后 #130 关闭；#131 仍受 CE01 单段 flat 镜像限制，不在本路线解决。
