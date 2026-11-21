# CatOS v2.67 安全审计：怀疑一切

> 本文对应 `kernel_v25/src/sec.c`、`posix.c`、`hive.c` 与 `shell.c` 分发器里的实际代码。
> 运行时同一份结论可以用 `audit` 命令打印（策略快照 + 决策日志）。

## 1. 威胁模型

CatOS 是单地址空间的内核：ring0 里跑 shell builtin，ring3 里跑 `.ce`。没有 MMU 隔离、
没有 NX、没有 ASLR、没有签名体系。因此**不能声称"防病毒"**，只能做到三件事：

1. 让"谁可以执行什么"成为**可配置、可审计**的显式策略，而不是散落在各命令里的一行 `if`；
2. 让所有拒绝都有**统一出口与日志**（`sec_audit`），不出现静默失败；
3. 让"可疑输入"默认失败（fail closed），而不是默认放行。

## 2. 能力模型

| 能力 | 位 | 保护对象 |
|---|---|---|
| `NET` | 0x0001 | curl / ping / dns / udp / netcfg / netdiag |
| `KILL` | 0x0002 | kill |
| `FSADMIN` | 0x0004 | format / fsck / mount / umount |
| `AUDIO` | 0x0008 | sndtest / sndinfo |
| `USERADM` | 0x0010 | useradd / userdel / passwd / chcaps / su / chown / hive 写 |
| `DEVICE` | 0x0020 | 原始设备访问 |
| `SYS` | 0x0040 | shutdown / reboot |
| `FORMAT` `FSCK` `RAWIO` `HWINFO` `REBOOT` `DIAG` | 0x0080..0x1000 | FSADMIN / DEVICE / SYS 的细分位 |
| `ALL` | 0xFFFFFFFF | root 等价 |

判定规则：`uid == 0` 直接放行；其他用户必须**持有全部所需位**（不是任意一位）。
`sec_allow()` 在 `shell_exec` 分发之前统一调用，命令体不再各自判断。

### 深度自定义（无需重新编译）

```
hive set HKSEC\Policy\Cmd\curl\Caps      "NET|KILL"   # 提高要求
hive set HKSEC\Policy\Cmd\format\Caps    "none"       # 显式降权（危险，需 root）
hive set HKSEC\Policy\Cmd\shutdown\Caps  "NET"        # 改需求
hive save
```

- 键不存在 → 使用内置表；
- 值为空 / `none` / `guest` → 无要求；
- 值存在但**解析失败**（拼错、未知名） → `SEC_HOSTILE`，命令一律按"权限不足(13)"拒绝。
  这样一份被写坏的 hive 永远不会"意外放开"权限。

## 3. 可执行文件策略

`run_from_path()`（cwd 优先，然后 `$PATH`）在 `spawn` 之前调用 `sec_exec_check()`：

- `HKSEC\Policy\Exec\Strict=1`（默认）时，拒绝**world-writable / group-writable** 的 `.ce`；
  这是"放个文件到共享目录就能以启动者身份执行"的经典提权路径。
- 每次外部执行都写 `exec-ce` 审计记录（含绝对路径）。
- 内置命令永远优先于同名文件，所以 `ls`、`cat` 这类核心命令无法被同名 `.ce` 劫持。

## 4. 前台任务独占（用户说的"强制不能操作"）

- 任何 builtin 在 ring0 同步执行，**不主动让出 CPU**；键盘 IRQ 仍在缓冲按键，但 shell
  要等命令返回才读取，因此一个死循环能长期占住控制台。`hang` 就是刻意演示这一点的命令。
- 只有自行轮询 `kb_getkey_timed()` 的命令才响应 Ctrl+C（`yes`、pager、`sleep`）。
- 目前**没有**看门狗、没有每任务 CPU 配额、没有"强制夺回控制台"的路径。
  缓解手段只有 Ctrl+Alt+Del 与物理复位。这是已知缺口，不是已修问题。

## 5. 已知攻击面（未修）

| 面 | 说明 | 现状 |
|---|---|---|
| `/etc/autoexec` | 开机以 root 执行脚本 = 持久化后门 | 目录应 root 0755；`ls -l` + `chmod` 自检 |
| `pkg install` | v2.68 起为 CPKG02 + RSA-512 验签（`pkg_verify`），签名不符拒绝安装；私钥不进内核 | 只从自己控制的目录安装 |
| 无隔离 | `.ce` 与内核同地址空间 | 架构性限制 |
| 无签名 | `UnknownBinary.AllowUnsigned=1` 默认允许 | 策略键存在，校验器尚未实现 |
| 中断风暴 | 旧 ES1370 驱动不使能 INTEN；若使能会 IRQ storm | 驱动层已规避 |

## 6. 审计日志

`sec_audit(uid, kind, what, rc)` 写入 48 条定长环形缓冲（tick / uid / 结果 / 类型 / 命令）。
`audit` 打印策略快照与最新条目，`audit -n N` 限制条数，`audit --clear` 清空。
记录类型：`exec`、`usage`（语法错误）、`deny`（权限拒绝）、`deny-exec`（可执行文件被拒）、
`exec-ce`（外部程序执行）。

## 7. 命令语法契约（posix.c）

统一 POSIX 语义，全部在命令体执行**之前**判定：

| 场景 | 输出 | 退出码 |
|---|---|---|
| 未知短选项 | `cat: invalid option -- 'z'` + `Try 'cat --help'` | 2 |
| 未知长选项 | `ls: unrecognized option '--bogus'` | 2 |
| 缺操作数 | `cat: missing operand` | 2 |
| 多余操作数 | `ps: extra operand 'x'` | 2 |
| 文件不存在 | `cat: /no/such: No such file or directory` | 1 |
| 权限不足 | `format: permission denied (need CAP_...)` | 13 |

`--help` 对表中所有命令自动可用（印出 synopsis + 允许的选项 + 退出码约定）。
