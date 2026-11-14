# 工件身份台账（哪个结论对应哪个 ISO / 镜像）

> SRE 用途：报告里每一条"修复前/修复后"的结论都必须能对上具体工件。
> 没有 hash 的结论视为未验证——**"源码看着是对的"不是证据**。

## 1. 关键时间线（2026-11-13）

| 时间 | 工件 / 事件 | sha256 | 备注 |
|---|---|---|---|
| 14:07:45 | 提交 `7e1fa09`（HEAD） | — | `git show HEAD:kernel_v25/src/task.c` 中**没有** `t->ring0 && !cur_task->ring0` 守卫 |
| 17:50:21 | `kernel_v25/src/shell.c` mtime | — | — |
| **17:51:55** | **`kernel_v25/catos.iso`** | `4dc87877beb6ac53d5a4a4e777a3d3fd3fe5a02ba986756cd39d446c06fdb2b0` | **红队 `crash_attack.py:51` 攻击的就是这个 ISO** |
| 17:53:21 | `kernel_v25/catos_disk.img` mtime | `39296ad859b59aceb8dfdc4a77f93f0856694232266cdf200b12ba685ed2d803` | 隔离核验 C 项的"攻击基线时点"hash |
| 18:03:36 | `tests/baseline-catos.iso`（蓝队 baseline 副本） | `4dc87877…`（与上同） | 就是 17:51:55 那份的副本 |
| **18:05:31** | **`kernel_v25/src/task.c` 改动落盘** | — | ring0 守卫 + 注释，**未提交**（`git diff -U0` 可见 `+` 行块） |
| 18:07–18:10 | 我的隔离核验 guest 运行（`:4494`） | 源镜像与 ISO 均在上述时点 | `isolation_logs/C_*.txt` / `D_*.txt` |

## 2. 由此得出的一条硬结论

**当前被攻击的 ISO（17:51:55，`4dc87877…`）早于修复代码落盘（18:05:31）14 分钟，
因此它不包含 ring0 守卫。** 本轮所有攻击现象都来自**未修复**的内核。

这不影响"找 bug"的有效性——打的确实是当前的权威工件。
但它对"验证修复"是致命的：**修完之后若仍用同一份 ISO 复跑，必然复现**，
报告会出现"源码已修 / 现象仍在"的矛盾，而且因为源码看起来正确而极难排查。

## 3. 规则（从现在起所有人遵守）

1. **每次构建后记录新 ISO 的 sha256**，写进日志与报告；
2. 报告里每条结论副署工件：`ISO=4dc87877… / DISK=39296ad8…`；
3. **复验前先比对 hash**：hash 没变 = 重测，不是复验；
4. 定级与销项**只看被测工件的行为**，不看工作区源码；
5. 本台账由我维护；蓝队构建完成后我补一行新 hash 并重跑隔离核验。

## 4. 待补

- [ ] 蓝队最终权威构建后的新 `catos.iso` / `catos_disk.img` hash（等 team-lead 通知）
- [ ] 用新工件重跑 `python tests/isolation_verify.py`，更新 `C_disk_isolation.txt` 的基线 hash
- [ ] 红队"修复后验证"轮次的工件 hash 与结论

## 5. 已补（2026-11-13 18:36–19:00）

| 时点 | 工件 | sha256 | 结论 |
|---|---|---|---|
| 最终构建 | `kernel_v25/catos.iso` | `65b326f6ecabb8c8…` | 隔离核验 **10/10 PASS** |
| 最终构建 | `kernel_v25/catos_disk.img` | `9361a29db20fc428…` | C 项：源不变 / 副本变 |
| 冻结基线 | `tests/baseline-catos.iso` | `4dc87877beb6ac53…` | 未变（证据链完整） |

### 5.1 A10 向量定案（两个工件各一次运行）

| 运行 | ISO | 结果 |
|---|---|---|
| `a10_intno.py --baseline` | `4dc87877` | **`intno=14`(#PF)、`err=0x2`、`cr2=0x7cfffffd`、`task=ATKGFX`、故障 eip=`0x01005d48`、`valid=1`** → V-03 根因定案 |
| `a10_intno.py`（默认新工件） | `65b326f6` | `valid=0`、空闲态（`EIP=0x01000310`、IF=1）→ **不再 fault**，V-03 修复首个实测证据 |

### 5.2 可复现性缺口（必须写进报告）

**`tests/attack_disk.img` 不是自包含的**：它只带红队最近一次 `prepare_disk()` 注入的 CE 集合。
直接拿它复现 A10 会得到 `run: cannot load '/bin/ATKGFX.CE' (err -2)`。
正确复现路径：`kernel_v25/catos_disk.img`（pristine）→ `tests/inject_any.py` 注入
`kernel_v25/extapp/bin/atk_gfx.ce` 为 `ATKGFX.CE` → 用对应侧 ISO 启动。
本文的读数即按此路径取得（`tests/a10_intno.py` 已固化该流程）。
