# 红队（泰莎）崩溃攻击实测结果 — CatOS v2.5

**攻击基线工件**：`kernel_v25/catos.iso` sha256 `4dc87877…`（2026-11-13 17:51:55 构建，不含任何蓝队修复）
**磁盘基线**：`kernel_v25/catos_disk.img` `39296ad8…`（17:53:21）
**驱动**：`tests/crash_attack.py`（端口 4492，`-net none`，`-display none`，每个攻击一份独立磁盘副本）
**攻击源码**：`kernel_v25/extapp/attacks/*.c`（11 个 CE + `craft_bad_ce.py` 生成 10 个畸形 CE01 头）
**证据**：`tests/attack_logs/<ID>-<name>.txt`（屏幕原文 + `faultinfo` 探针 + `info registers`）、`summary.json`

## 0. 一句话结论

16 组攻击中 **5 组把整机打到不可用**（A04 内核不可抢占忙等 / A10 ring0 页故障停机 /
A06 打死 shell / A08+A09 与 shell 互等死锁），**11 组被防住**；另有 **1 组静默数据破坏**
（A13：`file_write` 一个合法调用把 `/BIN` 从 36 项削到 2 项）和 **2 条纵深缺陷**
（A05 VMA 回绕、A02 可写 vsyscall 页）。

`panic()` 是死代码（`src/diag.c:83`，零调用点），"KERNEL PANIC" 永不出现；
真实的"击溃"标志只有 `ring0 kernel fault - system halted`（A10）或静默挂死（A04/A08/A09）。

## 1. 发现表（按严重度）

| ID | 攻击向量 | 判定 | 关键证据（屏幕/寄存器原文） | 复现命令 | 疑似根因 |
|---|---|---|---|---|---|
| **A10** | `display_mode(3)` 后 `gfx_rect{x=0x7FFFFFFF,y=0,w=1,h=1}` | **C1 内核停机** | `CR2=0x7cfffffd`（= `LFB 0xFD000000 + 0x7FFFFFFD`，即 `x*stride` 截断）；`EIP=0x0101cd61`（符号表：`_isr_exception` 0x0101ccd0..ce40，`cli/hlt` 停机循环）；`HLT=1 IF=0 CPL=0`，4 次采样 EIP 差 0 | `run /bin/ATKGFX.CE` | `src/task.c:1760` 只校验 `w/h`，**不校验 x/y**；`src/be_vbe24.c:139 clip()` 的 `*x + *w > GFX24_W` 有符号溢出 → `fill()` 用 `x=0x7FFFFFFF` 算地址 → ring0 #PF → 停机 |
| **A04** | `alloc/free/brk`：double free + 伪造 `heap_hdr`（栈内 size=0xFFFFFFF8） | **C1 内核不可抢占忙等** | `a=0x02100008` 连续 8 次（自由链自环）；`brk=0x00000000 ->=0x02100000` 后 `alloc(4K)` **无返回**；`EIP=0x0102171e`（反汇编：`mov (%esi),%eax; cmp %ebx,%eax; jb` = `sys_alloc` 自由链遍历被内联进 `_isr_syscall`）；`IF=0 HLT=0 CR2=0` | `run /bin/ATKHEAP.CE` | `src/task.c:864 sys_free()` 无 double-free/环检测，且只查 `vma_contains` → 栈/镜像指针可入链；`:825-860 sys_alloc()` 遍历无圈数上限。`int 0x80` 是 0xEE 中断门（`src/intr.c:223`）→ 死循环内 `IF=0`，时钟中断进不来，**无法抢占/唤醒** |
| **A06** | `api->kill(0)`（TID 0 = ring0 shell） | **C5 控制台永久消失** | `[kill: tid 0 (shell)]` / `kill(0)=0` / `ATK-DONE` / `[task 1 'ATKPROC' exit(0)]`；随后 `faultinfo` 探针**零内核应答**；CPU `EIP=0x01000310 IF=1 HLT=1`（内核还活着，只是没 shell 了） | `run /bin/ATKPROC.CE` | `src/task.c:381 task_kill()` 只拒 `tid==cur_task` 与 uid 不符，**不拒 ring0/不拒 TID_SHELL**；root 子进程 uid 检查直接放行。`shell_run()` 是内核主循环且不返回 → 不可恢复 |
| **A08** | `api->wait(gettid())` 自我等待 | **C3 死锁** | 停在 `WAITSELF-BEFORE`；探针无应答；`EIP=0x01000310 IF=1`（idle，无可运行任务） | `run /bin/ATKWAIT.CE 1` | `src/task.c:1618 SYS_WAIT` 不校验 parent；`task_wait_child()`（:400）对非子任务无限 `task_block`；shell 正在 `T_WAIT_CHILD` 等我们 → 互等 |
| **A09** | `api->wait(0)` 等 shell | **C3 死锁** | 停在 `WAIT0-BEFORE`；探针无应答；同 A08 的 idle 特征 | `run /bin/ATKWAIT.CE 2` | 同上（同根因，不同入口） |
| **A13** | FS 参数滥用；含 `file_write("/BIN", …, mode 0)` | **C5 静默数据破坏** | `fw-dir=0`（**成功**）；随后 `ls /BIN` 只剩 **2 项**（开机 `ce_install_builtins()` 装 36 个）、`fsize /BIN/HELLO.CE=-1`、`exists=0` | `run /bin/ATKFS.CE` | `src/task.c:1469 SYS_FILE_WRITE` → `fs_write_raw_mode()` 不判断目标是否为目录（无 `S_ISDIR` 检查）→ 用数据覆盖目录项/簇链 |
| **A05** | `munmap(addr, 0xFDBFE000)`：`addr+len` 32 位回绕 | **C5 内核状态破坏** | `munmap wrap=0`（**成功**）；`brk now=0x02100000` 正常；随后低地址 syscall 全 `-3`（`validate_uaddr` 已放行，被 `pt_for()` 的 PDE 8/9 限制挡住）；攻击者自己 `page fault cr2=0x00100000` | `run /bin/ATKMMAP.CE` | `src/vma.c:243` `addr + ((len+0xFFF)&~0xFFF) > USER_EXT_END` 可回绕；`:258 v->start = end` 不校验 `start<end` → VMA 被改写为 `[0, v->end)`。**未升级为写内核**：所有用户拷贝都经 `task_virt_to_phys()`，只认 PDE 8/9 |
| **A02** | 65 槽参数毒化（含 `strcpy(dst=0x2080000)`) | **OK** + 缺陷 | 攻击者在 `eip=0x02080009 cr2=0xd0dad6cf` 被自己杀死：`strcpy` 把字符串写进了 **vsyscall 页**，把后续 syscall thunk 改坏 | `run /bin/ATKARGS.CE` | `src/task.c:2153` vsyscall 页映射 **RW**（注释自称 "user read-only"）；`:2117` CE 镜像也映射 RW（W^X 失效）。只影响自身（ring3 代码），无提权 |
| A01 | 非法 syscall 号 65..0xFFFFFFFF + 非法 int 向量 | **OK** | `nr=0x41 r=-3`、`0x3e9..0xffffffff` 全 `-8`；`int $3` → `[task 1 'ATKSYSNR' killed: fault at eip=0x02000286]`，`faultinfo` = `gpf ring3`，prompt 回位 | `run /bin/ATKSYSNR.CE` | 无（`switch` + `default: -8` 有效；0..47 为 DPL0 门） |
| A03 | 长度/偏移溢出（cat/save/file_rw/hw_str/strlen/路径） | **OK** | 负长/巨长全 -3/-7/-22；64 KiB 上限生效；`ATK-DONE` | `run /bin/ATKLEN.CE` | 无（`max<=0`、`>cap` 都被钳位） |
| A07 | `wait()` 垃圾 tid / 子进程正常回收 | **OK** | `ATK-DONE` | `run /bin/ATKWAIT.CE` | 无 |
| A11 | 管道句柄/长度/重复 close/write-after-close | **OK** | `ATK-DONE`；越界 handle 全 -1 | `run /bin/ATKPIPE.CE` | 无 |
| A12 | 网络畸形输入（无网卡负向验证） | **OK** | `init=-1 up=0`；畸形 URL/398 字符主机名/巨长 max 全 `-1` | `run /bin/ATKNET.CE` | 无（无 NIC 短路，属**环境假象**，不代表 netstack 安全） |
| A14 | ring3 深递归 600×512B 撞 128 KiB 栈 | **OK** | `[task 1 'ATKSTACK' killed: page fault]`，prompt 回位 | `run /bin/ATKSTACK.CE` | 无（栈 VMA 边界正确，ring3 故障只杀任务） |
| A15 | 8 MiB VLA | **OK** | 同上（ring3 页故障） | `run /bin/ATKSTACK.CE vla` | 无 |
| A16 | 10 个手工畸形 CE01 头 | **OK** | `BAD1/2/3/4/6/7/8/9` → `cannot load (err -5)`；`BAD5`/`BADA` 属**故意合法头**（BAD5 api_offset=img-4 是合法最大值，BADA 头合法但代码是 int3）→ 正常加载后在 `eip=0x02000000` 被 ring3 故障杀死 | `run /bin/BADn.CE` | 无（`img+20>size`、`api>img-4` 的无溢出比较有效） |

## 2. 统计

- 已执行 **16/16**；确认击溃 **5**（A04 C1、A10 C1、A06 C5、A08 C3、A09 C3）
- 静默破坏 **2**（A13 卷数据、A05 内核 VMA 状态）
- 防守有效 **9**（A01/A02/A03/A07/A11/A12/A14/A15/A16）；其中 A12 是"无网卡短路"的环境假象，A13 的 fs 向量由本条另计
- 未复现（尝试但没打崩）：非法 syscall 号、参数毒化（除 vsyscall 自伤）、长度/偏移溢出、CE 头畸形、管道、进程表耗尽、ring3 栈耗尽

## 3. 与已存在报告的差异（需以本条为准的部分）

1. **A16 的 "BAD5/BADA 未被拒绝" 不是 bug**：这两个样本是我按"合法头"故意构造的对照（`api_offset = image_size-4` 是加载器允许的最大合法值；BADA 头合法、正文为 `0xCC`）。9/10 应以 `-5` 拒绝，2 个执行后由 ring3 故障杀死 = **正确行为**。蓝队不要为此改加载器。
2. **A13 早期结果（`killed at eip=0x00000000`）是我攻击程序的 bug**：`api->ls(path, NULL, ud)` 会在 ring3 thunk 里 `call 0` 直接打死调用者 → 内核无责，但"NULL 回调 = 杀自己"本身是个 ABI 健壮性缺陷（应返回 EINVAL）。修正回调后重跑，FS 向量才真正执行，并因此发现 **`file_write` 到目录会成功并摧毁目录**。
3. `tests/attack_logs/summary.json` 现已 **16 条齐全**（A01 为 OK，A16 为 OK），早期"13 条 / A01=C5"是中途落盘快照。

## 4. 环境说明（避免误判）

- 本 guest `-net none`：A12 全部为错误码，**不能**据此认为 netstack 抗打；要测需另开带 `-netdev user` 的隔离环境。
- 本 guest 无 SB16：`snd_*` 槽全 `-8`（A02 覆盖），**音频向量未做真实覆盖**。
- `-no-reboot`：真复位会让 QEMU 退出，驱动据此判 C2（本次 0 例）。
- A10 之后控制台在 800×600×24 图形模式，VGA 文本缓冲不再刷新，**屏幕为空不等于没崩**；A10 的判定完全基于寄存器（`HLT/IF/EIP/CR2`）。
