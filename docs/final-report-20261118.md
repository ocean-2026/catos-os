# CatOS 跨夜任务收官报告（2026-11-17 22:12 启动 → 2026-11-18 收官）

- 主仓库：`/home/user/Doubao/chats/38453665424926722/CatOs`
- 收官代码基线：`86d0ab7`（docs: record R1 blue fixes #163-#187 and deferred #188-#193）；本报告为其上的 docs-only 提交（hash 见 `git log` 顶部）。
- 基线 ISO：`kernel_v25/catos.iso`；基础磁盘：`kernel_v25/catos_disk.img`
- QEMU：`/home/user/bin/qemu-system-i386`（显式 `--qemu` 传入）；本批探针端口 4500–4509。
- 构建：`cd kernel_v25 && sh build.sh iso`。

---

## 一、六任务对照表

| # | 版本 / 轮次 | 任务 | 交付内容 | 落地 commit | 状态 |
|---|---|---|---|---|---|
| ① | v2.52 | shell 作业控制最小闭环 | `fg`/`bg`/`stop` 内建、`T_STOPPED` 状态机、最小信号抽象、Ctrl+Z 前台挂起；修复退出路径 autoreap/wake 顺序竞态（#156） | `e0b1cac` | ✅ 完成，jobctl 回归绿 |
| ② | v2.53 | 多组支持（附加组） | 持久化 `/etc/group` 组库；权限遍历主 gid/附加组任一命中；`groupadd`/`groups` 内建；修复 mode triad 与 PERM 位掩码混用（#157） | `89ab49c` | ✅ 完成，group_regress 10/10 |
| ③ | v2.54 | 轻量守护原语 | `task_t.daemon` 标记、`daemon` 内建、`/etc/autoexec` 行首 `@` 守护启动、示范守护 `memlog`；jobctl/daemon 探针竞态修复（settle/clear/长寿命参数），spinner 上限 200 | `3091719` | ✅ 完成，daemon_regress 全 PASS |
| ④ | 蓝队安全修复 R1 | 对抗 R1 发现项修复 | 红队/白队发现中可落地项共 15 项（#163–#187），13 个 fix commits；另登记 deferred #188–#193 | `0a49d9d`…`a7d475b`（见第三节版本链） | ✅ 可落地项全修；6 项 deferred |
| ⑤ | v2.55-ttf | TrueType 最小解析层 | sfnt 目录 / cmap format 4 / glyf 简单+复合字形 / head-hhea-hmtx 度量，全定点 F2Dot14，边界校验；宿主单测与 fontTools 逐点交叉核对；DejaVu/Liberation ASCII 子集入库 | `d2f980e`、`9f0b907`、`1c50c6e` | ✅ 解析层+交叉核对完成；内核渲染集成 deferred |
| ⑥ | 对抗测试基建 R1 | 红队动态攻击 + 白队静态审查 + 蓝队修复闭环 | 磁盘副本隔离（攻击不污染基础盘）、独占 monitor 端口、findings 归档（`adversarial/red_findings_r1.md`、`white_findings_r1.md`、`red_evidence_r1/`）、问题清单续号 #158+ | 归档于 `adversarial/`；修复落主仓库 | ✅ R1 闭环完成 |

---

## 二、版本链

跨夜起点 `6cf1ff6`（docs: 审美审查报告补登 v2.45 整改 commit hash e839830）→ 本轮功能与修复链：

| commit | 版本 / 内容 | 时间 |
|---|---|---|
| `6cf1ff6` | 跨夜起点（v2.45 审美整改 hash 补登） | 11-17 夜 |
| `e0b1cac` | v2.52：shell 作业控制最小闭环（#156 退出路径竞态修复） | 11-18 03:50 |
| `89ab49c` | v2.53：多组支持（附加组 + `/etc/group`，#157 掩码翻译修复） | 11-18 04:42 |
| `3091719` | v2.54：轻量守护原语（daemon 标记 / `daemon` 内建 / `@` 自启 / memlog 示范） | 11-18 05:21 |
| `d2f980e` | v2.55-ttf：TrueType 最小解析层（sfnt/cmap format4/glyf 简单+复合/度量，全定点 F2Dot14），宿主单测 fontTools 逐点核对 | 11-18 06:18 |
| `9f0b907` | 示例字体入库：DejaVu/Liberation ASCII 子集（16.8KB / 27.4KB），交叉核对扩展至入库子集 | 11-18 06:19 |
| `1c50c6e` | 复合字形交叉核对（U+00E9）+ scale 标志位 / byte-args 解码修复 | 11-18 06:25 |
| `0a49d9d` | fix(#163)：group db init 缺成员列 NULL-guard | 11-18 06:31 |
| `3c1fc90` | fix(#164/#178/#166)：group/acl/user db save 有界追加 | 11-18 06:32 |
| `d415332` | fix(#167)：重定向目标预建/预截断前校验写权限 | 11-18 06:33 |
| `da89050` | fix(#168)：rmdir 补 PERM_WRITE 闸门 | 11-18 06:34 |
| `c2bd2d4` | fix(#182)：编辑器打开/保存与 PE→CE 转换落盘权限闸门 | 11-18 06:35 |
| `9988828` | fix(#165)：group_save 逗号按已输出成员判定 | 11-18 06:37 |
| `0b6db38` | fix(#181)：能力授予约束在授予方所持位子集内 | 11-18 06:38 |
| `4de63b8` | fix(#180)：spawn 失败回滚释放 VMA 节点链表 | 11-18 06:39 |
| `ff99325` | fix(#170)：abs_path 有界拼接，超长路径拒绝 | 11-18 06:41 |
| `b4e11c3` | fix(#184)：kill 分发传会话 uid，不再硬编码 UID_ROOT | 11-18 06:51 |
| `bc542d3` | fix(#185)：非 hex 口令字段保留为未知口令，不再清空解锁 | 11-18 06:51 |
| `f6be766` | fix(#186)：groupadd 先校验成员、失败撤空槽、溢出告警 | 11-18 06:51 |
| `a7d475b` | fix(#187)：加载 /etc/group 拒绝重复 gid | 11-18 06:51 |
| `5160874` | build：memlog 加入 apps.list（daemon 回归应用） | 11-18 07:12 |
| `fa13575` | test：user_regress 与其余套件一致支持 `--port` | 11-18 07:12 |
| `86d0ab7` | docs：登记 R1 蓝队修复 #163-#187 与 deferred #188-#193 | 11-18 07:22 |

上述链路上的 commit 均已 push `origin/main`（remote：`https://github.com/ocean-2026/CatOS.git`）。

---

## 三、对抗 R1 统计

测试基线：HEAD `3091719`（v2.54）；每轮攻击前 `cp` 基础磁盘到 `/tmp/atk_work/` 副本运行，未污染基础磁盘；QEMU `-m 256 -no-reboot -net none`，monitor 端口独占。

### 3.1 红队动态攻击（`adversarial/red_findings_r1.md`）

- 合计 **16 条**：**P0 = 0**，P1 = 1，P2 = 7，P3 = 8（含多条负向确认/观察项）。
- 唯一 P1：RED-03 —— `kill` 内建硬编码 `UID_ROOT`，caps=0 非 root 会话可杀死 root 守护进程（已修 → #184）。
- P2：RED-02（TID 槽位复用无代际，→ #188 deferred）、RED-05（重复 gid 串显，→ #187 已修）、RED-06（groupadd 失败落库，→ #186 已修）、RED-09（守护直写控制台刷屏，→ #189 deferred）、RED-11（超长组件名静默截断，→ #190 deferred）、RED-14（非 hex 口令被解锁，→ #185 已修）、RED-15（猜测回答 n 不清待确认态，→ #191 deferred）。
- P3：RED-01/07/08/10/12/13/16 负向确认与观察，RED-04 跨会话枚举（→ #193 deferred）。

### 3.2 白队静态代码审查（`adversarial/white_findings_r1.md`）

- 合计 **31 条**：P1 = 5，P2 = 11，P3 = 12（含观察/核对结论类）。
- P1：WHITE-008（group 两字段行未初始化栈指针解引用，→ #163 已修）、WHITE-009（group_save 静态缓冲无界写，→ #164 已修）、WHITE-013（重定向预建绕过权限，→ #167 已修）、WHITE-014（rmdir 缺权限检查，→ #168 已修）、WHITE-030（edit/PE→CE 落盘无权限闸门，→ #182 已修）。
- P2/P3 中可落地项：#165（前导逗号）、#166/#178（user/acl save 无界写）、#170（abs_path 栈溢出）、#180（回滚泄漏 VMA 链表）、#181（能力授予无子集约束）等均已修；#188/#189/#192/#193 等归 deferred（见第五节）。

### 3.3 蓝队修复

- 可落地问题 **15 项（#163–#187）全部修复**，由 13 个 fix commits 落地（`3c1fc90` 一单覆盖 #164/#178/#166）；另 2 个 build/test 支撑 commit（`5160874`、`fa13575`），1 个 docs 登记 commit（`86d0ab7`）。
- 登记 **deferred 6 项：#188–#193**（改动面大/需专项设计，见第五节）。
- 证据归档：`adversarial/red_evidence_r1/`；findings 全文：`red_findings_r1.md`、`white_findings_r1.md`。

---

## 四、回归结果

- R1 修复后的全量终验回归由并行子代理执行，结果写入：
  `/home/user/Doubao/chats/38453665424926722/adversarial/regression_final.md`。
- **终验已完成：152/152 PASS，0 FAIL / 0 WARN / 0 ERR，14 套件退出码全 0**（qemu 12、guess 16、fsrec 12、lfn 14、recur 10、jobctl 12、group 10、daemon 5、caps 12、user 20、disk 5、env 13、heap 4、size 7），未发现合流（3091719..86d0ab7）引入回归；被测代码态 86d0ab7（ea60ba9 仅 docs 不受影响）。原始日志 `tests/regress_logs/final_<suite>.log`。
- 本轮开发期内既有套件基线：jobctl / group / daemon / lfn / user / gfx24 等针对性回归在各自落地 commit 时复跑通过；全量十二套 sweep 基线记录见 `docs/` 既有回归文档。

---

## 五、问题清单 #153–#193 状态对照

来源：`docs/CatOS-内核问题清单.md` 现状整理。

### 5.1 已修复

| # | 严重度 | 问题摘要 | 修复 commit |
|---|---|---|---|
| 153 | 中 | ATA 空槽 IDENTIFY 状态恒定无跃迁，探测空转烧时间（四槽约 1.65s 启动开销） | v2.46 稳定无信号采样 + 兜底收紧 |
| 154 | 低 | GRUB 模板 `timeout=5` 菜单倒计时占 pre-kernel 约 5s | v2.46 组装期替换 `timeout=0` |
| 156 | 中（竞态） | 退出路径先唤醒 parent 再 autoreap，等待方在已回收槽位死循环 | v2.52：先 reap 判定后唤醒，wait 防御性接受 T_UNUSED |
| 157 | 中 | mode triad（r=4/w=2/x=1）与 PERM_* 位掩码（1/2/4）混用，组/主读位误拒、执行位误放 | v2.53：triad 先翻译成 PERM_* 再比对 |
| 163 | 中 | `group_db_init` 两字段行解引用未初始化栈指针；`split_fields` 未填槽位未置 NULL | `0a49d9d` |
| 164 | 中 | `group_save` 静态缓冲无界写（分隔符/数字/换行） | `3c1fc90` 有界追加 |
| 165 | 低 | `group_save` 已删首成员留前导逗号 | `9988828` 改 emitted 标志 |
| 166 | 中 | `user_save` 同类静态缓冲无界写 | `3c1fc90` |
| 167 | 中 | 重定向预截断/预建文件绕过 `fs_perm_check` | `d415332` 预操作前校验 PERM_WRITE |
| 168 | 中 | `cmd_rmdir` 完全缺权限检查 | `da89050` 补 PERM_WRITE 闸门 |
| 170 | 中 | `abs_path` 无界 scpy/scat 撑爆 320 字节栈缓冲 | `ff99325` int 返回 + 有界拷贝，超长拒绝 |
| 178 | 中 | `acl_save` 同类静态缓冲无界写 | `3c1fc90` |
| 180 | 中 | spawn 失败回滚不释放 VMA 节点链表，复用槽位残留陈旧链表 | `4de63b8` 新增 `task_free_vma_list` |
| 181 | 中 | 能力授予无子集约束，USERADM 账户可自授 CAP_ALL | `0b6db38` 非 root 按所持位裁剪 |
| 182 | 中 | 编辑器打开/保存与 PE→CE 转换落盘无权限检查 | `c2bd2d4` 落盘前校验会话身份 |
| 184 | 高 | kill 分发硬编码 UID_ROOT，绕过属主/CAP_KILL 判定（RED-03） | `b4e11c3` 传会话 g_uid |
| 185 | 中 | 非 hex 口令字段被清空致账户解锁（RED-14） | `bc542d3` 无法解析保留未知口令 |
| 186 | 中 | groupadd 成员校验失败不撤空槽、超容量 token 静默丢弃（RED-06） | `f6be766` 先校验后建槽、失败撤槽、溢出告警 |
| 187 | 中 | 磁盘 /etc/group 加载不做 gid 唯一性校验，跨组串显（RED-05） | `a7d475b` 遇已见 gid 跳过 |

### 5.2 Deferred（登记留后续，本轮不改）

| # | 严重度 | 问题摘要 | defer 原因 / 后续方向 |
|---|---|---|---|
| 155 | 中（latent） | fat 嵌套递归删除时 delete-name 重入，目录簇静态缓冲被复用覆盖 | 目录簇读缓冲需改调用方传入/堆上分配；在此之前 `rm -r` 依赖 force 变体保持基线行为 |
| 188 | 中 | TID 槽位即时复用无代际/已退出标记，旧 tid 静默命中新任务（RED-02 / WHITE-001 / WHITE-002） | 改动面大（任务表/作业控制/所有 tid 引用命令）；后续引入代际计数或退出标记 |
| 189 | 中 | 守护任务存在绕过 ring 隔离的直写控制台路径，刷屏淹没提示符（RED-09） | 需先定位 memlog 计数走的 syscall/直写入口，再统一并入 per-task 环形缓冲 |
| 190 | 中 | 组件名超 `FS_NAME_MAX(48)` 未被拒绝，静默截断生成退化 8.3 别名（RED-11） | 与长名截断/8.3 别名生成联动；后续在写侧入口做组件名长度校验并拒绝超长 |
| 191 | 低 | did-you-mean 对回答 `n` 不清待确认态，`n` 被重新 arm，后续 `y` 回放新猜测（RED-15） | 状态机显式识别 n/no 并清除 pending offer |
| 192 | 中 | 管道句柄全局命名空间无属主校验，任意 ring3 任务可读写/关闭他人管道（WHITE-027） | 后续为管道句柄加属主任务校验（POSIX 式句柄私有归属） |
| 193 | 低 | `jobs`/`ps`/`log` 跨会话枚举其他属主后台/守护任务（RED-04 / WHITE-031） | 信息泄露不破坏内存；后续列表按属主过滤（root/属主/CAP_KILL 可见他人） |

> 说明：白队 31 条中部分观察/核对结论（如 WHITE-007/012/016/018/025 等）经核对确认当前无直接命中面，未单独立号；编号 #158–#162、#169、#171–#177、#179、#183 为评审过程中合并/折叠到相邻修复或观察项的编号段。

---

## 六、TTF 交叉核对结论（v2.55-ttf）

### 6.1 核对口径与结果

- 参考字体（宿主全量）：`DejaVuSans.ttf`、Liberation Sans；入库子集资产：`kernel_v25/fonts/DejaVu-subset.ttf`（16.8KB）、`kernel_v25/fonts/LiberationSans-subset.ttf`（27.4KB）。
- 核对字符：`A`（U+0041）、`g`（U+0067）、`é`（U+00E9，复合字形）。
- 判据：解析轮廓点与 fontTools 4.68.0 地面真值**逐点 byte-exact**（坐标 ≤1 font unit 误差内），advance width 相等；畸形/截断/垃圾缓冲全部安全拒绝。
- 结果：**4 字体 × A/g/é 全部逐点一致，宿主单测 ALL PASS**（含 unitsPerEm/numGlyphs 度量核对与畸形输入拒绝用例）。
- 复跑：`gcc -std=c99 -I kernel_v25/include tests/ttf_host_test.c kernel_v25/src/ttf.c`；期望表由 `tests/ttf_gen_expect.py`（fontTools）生成入 `tests/ttf_expect.h`。

### 6.2 调试累计修复的 7 个解析 bug

1. **rd8 字节偏移**：复合/简单字形字节参数初版误取 rd16 高字节；改为按低地址字节直读。
2. **cmap 子表相对偏移**：子表偏移相对 cmap 表起点（非文件绝对偏移），初版按绝对地址定位导致 format4 子表查找错位。
3. **REPEAT 标志展开循环条件**：标志字节 `REPEAT`（0x08）后随重复计数字节，展开循环的计数/推进条件初版错误，点位数与标志数组错位。
4. **复合 BYTE 参数分支**：无 `CF_WORDS` 标志时偏移量应为 `rd8×2` 共 2 字节；初版误按 `rd16×2` 共 4 字节推进，后续组件字段全部错读。
5. **零 delta 规则**：`X_SAME`/`Y_SAME` 在无对应 `X_SHORT`/`Y_SHORT` 时 delta 为 0、占 0 字节；初版漏该分支，多吃字节导致坐标流错位。
6. **scale 标志位错位**：复合变换标志 `WE_HAVE_A_SCALE=0x0008`、`WE_HAVE_AN_X_AND_Y_SCALE=0x0040`、`WE_HAVE_A_TWO_BY_TWO=0x0080`；`0x0100` 是 `WE_HAVE_INSTRUCTIONS`（指令跟随所有组件之后），初版被误判成 TWO_BY_TWO。
7. **期望发射循环漏发**：交叉核对期望生成器逐字符发射循环漏发部分字形，子集 cmap 缺字未跳过；修正为按每字体 nchars 迭代、缺字符跳过。

### 6.3 TTF deferred（如实登记）

- **内核集成未做**：`build.sh` 尚未接入 `ttf.c`，gfx24 `glyph_render` API 未对接；解析层目前仅在宿主单测验证。
- scale / 2×2 复合变换的实机渲染用例未做；CFF（PostScript 轮廓）字体不在本批范围，仅做 sfnt 目录识别与表存在性标注。

---

## 七、遗留与交接

1. 工作树中 40 余个 `.ce`/`.catpkg` 改动为构建/测试产物噪声，保留未提交；zip 打包时已排除产物目录。
2. 终验回归已完成（152/152 全绿），详见 `adversarial/regression_final.md` 与本报告第四节。
3. TTF 渲染集成、#188–#193 deferred 项、fat 嵌套删除重入（#155）列入下一轮路线。
