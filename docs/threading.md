# 用户态多线程（Stage 3，feat/hosted）

## 3A-3 / 3A-4：per-thread TLS + 四线程隔离（已落地，实机实测）

槽 83–86 从 `ENOSYS` 改为真分发，工作线程**共享父进程地址空间**跑通 create /
exit / join / detach。实现见 `src/task.c` 线程子系统块（`thread_create` /
`thread_exit_current` / `thread_join` / `thread_detach` / `thread_reap` /
`tls_apply`），探针 `extapp/t1.c`、`extapp/threads.c`。

### 资源归属（`thread_create`）

先把父进程共享字段拷过来（顺序关键：必须先建活目录，后续 anon mmap / 用户拷贝
才能经父目录映射）：`space`（`dir/pt/pt2` 结构拷贝）、`vm` 链表指针、`heap_end`、
`heap_free_root[slot]`、`uid/gid/caps`、`out_pipe/in_pipe/out_append/bg_mode`。
随后线程私有资源：

| 资源 | 大小 | 来源 |
|---|---|---|
| 内核栈 | 8 KiB | kvheap extent（`kvmalloc`），零页 |
| 用户栈 | 16 KiB | 扩展 arena 匿名 VMA（guard 在前） |
| TLS 用户页 | 4 KiB | 扩展 arena 匿名 VMA，**用户可写映射**（supervisor-only 的 KV 堆窗口不能当 TLS） |

TLS 页布局（32 位字）：`[0]=tid`、`[4]=errno（创建置 0）`、`[8]=0xC0FFEE` canary。

### TLS 走 MSR（不新增 GDT 描述符 / LDT）

- 特征门：`cpu_msr_supported()` 读 CPUID leaf1 EDX bit5 判 MSR 支持；不支持时
  `tls_apply()` 静默跳过（LDT fallback 未接线，见已知边界）。
- `IA32_GS_BASE = 0xC0000100`：`tls_apply(next)` 在 `schedule_to` 每次切换时
  `wrmsr` 写 `next->tls_va`；无 TLS 的任务写 0。`IA32_KERNEL_GS_BASE = 0xC0000101`
  已定义暂未用。

### ring3 帧构造

cdecl 用户栈：`[esp]=VS_EXIT_STUB` 返回地址、`[esp+4]=arg`。内核栈顶手搭
`int_frame_t`：`eip=entry`、`cs=SEL_UCODE`、`eflags=0x202`（IF=1）、`esp=用户栈顶`、
`ss=SEL_UDATA`、`ds/es/fs/gs=SEL_UDATA`；状态 `T_READY`、`resume_frame=1`、
`is_thread=1`、`proc=parent->proc`、`parent=parent`。

### 退出 / 回收状态机

- `thread_exit_current(retval)`：写 `thread_ret`；只 `vma_munmap` 自身用户栈与 TLS
  页，**绝不** `task_space_destroy` 拆共享 space。非 detached 置 ZOMBIE 并唤醒
  `waiter`；detached 置 ZOMBIE（其 kvheap 内核栈回收 deferred，见边界）。
- `thread_join(tid)`：找不到 / 非线程返回 `-22` EINVAL；非 zombie 则 `waiter=caller`、
  caller 阻塞 `T_WAIT_CHILD` 后 `reschedule()`；唤醒后取 `thread_ret`，调
  `thread_reap` 释放 kvheap 内核栈并置 `T_UNUSED`。
- `thread_detach(tid)`：置 `detached`；已 zombie 立即 reap。
- `thread_reap`：`kvmfree(kstack_va)`，清 `kstack_top/ksp/resume_frame/
  thr_stack_va/tls_va/waiter`，置 `T_UNUSED`；必须在非被回收线程上下文跑
  （要释放其内核栈）。

### 实机证据（本轮 mt2 构建）

- `t1.ce`：单 worker 打印 `T1` 后 `thread_exit(0)`，main join 得 0，`T1 PASS`。
- `threads.ce`：4 worker 各跑 1000 项私有栈数组并 `exit(id+1)`，main join 求和
  `sum=10`；detach 一个 worker 不 join；fault worker `exit(1)`，join 得 retval 1
  （非零退出不连坐进程空间）；终态 `THREAD PASS`。
- 三门禁回归（不回归基线）：vprobe 10/10、E3 2/2、F11 9/9，均 RC=0。
- strict 布局门禁 RC=0，`_bss_end=0x02117a40`；无新增大 `.bss` 数组。

### 已知边界（本轮 deferred，列入 3B/3C）

- **内核态仍非抢占**：PIT ISR 只抢占 ring3；ring0 协作式让出。多线程本轮只保证不崩
  隔离，不要求内核可抢占。
- **detached 内核栈 deferred 回收**：detached 线程退出置 ZOMBIE 但未在自身运行栈上
  立即释放 kvheap 内核栈；探针阶段 slot 占用 ≤16，泄漏在 3B/收尾处理。
- **TLS errno 的 CE 用户态访问器缺失**：内核已写 TLS `[4]=0`，但 CE 侧尚无
  `__tls_errno` 取址 / `mov %gs:off` 直读 thunk；探针 `errno=0` 为占位。
- **共享堆分配器未 per-process**：`heap_free_root` 仍按 slot 拷自父进程；线程内
  `malloc/free` 路径的归属/加锁列入 3B。
- 按 tid 索引的静态缓冲（`ls_buf`、`e3_*_buf` 等）在多线程下的串扰属 3B。
- MSR 不支持的 CPU 上 LDT TLS fallback 未接线；`kvheap inuse` 打点无对应 syscall 槽。

## 3A-2：`task_t` proc/thread 拆分脚手架（行为零变化）

本轮只做结构边界落地，**不接新槽、不迁字段**：

- `task.h` 前向声明 `struct proc`（进程描述符，持地址空间 / VMA / 堆根 / fd 表 /
  管道/控制台/日志归属）；`task_t` 作为线程描述符，新增进程回指 `proc` 与线程侧
  占位字段：`is_thread / detached / tls_va / thr_stack_va / thread_ret / waiter /
  kstack_va`。
- 既有进程（shell、spawn 出来的 CE 任务）按"单线程进程"映射：`proc=0` 表示隐式
  自持进程，`is_thread=0`。槽位复用与 `tasks_init` 都复位这些字段。
- 进程共享字段（`space/vm/heap_end/out_pipe/.../log_buf`）本轮**仍留在 `task_t`**；
  待 3A-3 工作线程真正共享地址空间时再迁入 `proc`，避免在无活线程路径时搬迁 150+
  处引用。
- 回归：strict RC=0（`_bss_end=0x02116a40`）；`dltest` PASS、`mmapfd` MMAP PASS。

## 3A-1：页后备虚拟连续堆（kvheap，已落地）

在拆 proc/thread 之前先打好通用内核堆地基。实现见 `src/kvheap.c` /
`include/kvheap.h`，映射原语 `paging_kmap/paging_kunmap` 在 `src/paging.c`。

### 虚拟窗口

```
0x18000000 .. 0x1a000000   kvheap 窗口，32 MiB（8192 帧），supervisor-only 写回
```

选窗依据：低于 4 GiB 物理上限；在 256 MiB 身份映射 RAM 顶（实测 `top=0xffe0000`）
之上，不被 PSE 身份映射覆盖；避开 CE 映像 `0x02000000` 与用户区
`0x2000000..0x2800000`。窗口 PDE 在 `kdir` 内建 4 KiB 页表，任务目录在建空间时
从 `kdir` 拷贝这些 supervisor-only PDE，所以在调用者 CR3 上内核仍可达窗口。

### 分配模型

- 接口：`void *kvmalloc(size, align)` / `void kvmfree(va)`；载荷 16 字节对齐，
  `align` 参数暂留。分配器只发虚拟地址，物理帧可碎。
- slab：`<=4096` 按 size class（32/64/128/256/512/1024/2048/4096）从共享帧切分；
  每类一条空闲链。
- extent：`>4096` 取窗口内一段连续 VA，逐帧映射零散物理帧；头部记帧数。
- 元数据：每类空闲链头 + extent 空闲链为固定小静态表，**未新增大 `.bss` 数组**。

### 启动自检

`kvheap_selftest()`：分配 8 KiB / 32 KiB 各一次，写满后读回比对；释放后重分配同尺寸
须拿回同一虚拟地址；再做一次 1 KiB slab 往返同址校验。串口：

```
kvheap selftest: PASS total=13 free=0 bytes=0
```

### 已知边界（本轮 deferred）

- extent 空闲跑在释放时**保留帧与映射**挂回空闲链（高水位缓存），帧池回收与 VA 跑
  紧凑化 deferred；进程/线程退出时的 extent 归还在 3A-3 接线程栈时再补。
- extent 单次最多 8 帧（32 KiB 量级）；超过需扩 `pa_tmp[]` 容量。
- 未做对齐大于 16、未做 OOM 回退到 slab 外的大块拆分。

## 结论

Stage 3 目标是在**单核、不碰 SMP** 的前提下给 CatOS 加用户态线程。3A 已落地：
kvheap 页后备堆、`task_t` proc/thread 拆分脚手架、槽 83–86 真线程分发（共享地址空间 +
per-thread MSR TLS）、四线程隔离探针，三门禁回归不回归基线。3B per-thread scratch 迁移、
3C 内核态可抢占仍**如实 deferred**，原因与下一步见文末。既有进程路径（spawn / wait /
pipe / console / heap / FPU）行为字节级未改。

红线延续：不动 v2.80 交付 ISO 与版本串；CE ABI append-only；不给应用可写像素出口；
33 fps 不动；不新增大 `.bss` 数组；`check_link_layout.py --strict` RC=0。

## 槽位（append-only，不可重排）

| 槽位 | 成员 | 签名 / 语义 | 状态 |
|---|---|---|---|
| 83 | `thread_create(entry, arg)` | `entry` 为 ring3 函数指针 `void *fn(void *arg)`；`arg` 经新线程用户栈传入。返回 tid>0 / 负 errno | 已实现（3A-3） |
| 84 | `thread_exit(retval)` | 终止调用线程；`retval` 作为 join 返回值 | 已实现（3A-3） |
| 85 | `thread_join(tid)` | 阻塞直到目标线程退出，回收其槽位，返回 `retval` | 已实现（3A-3） |
| 86 | `thread_detach(tid)` | 标记线程分离；退出时自动回收，无需 join | 已实现（3A-3） |

两处镜像（`include/ce_abi.h`、`include/ce.h`）成员顺序与 `SYS_*` 枚举序、`nargs[]`
表、vsyscall 表指针序一致：83 两参、84 一参、85 一参、86 一参。

## 现状内核模型（摸底结论，决定改造面）

当前 `task_t` 把**进程与线程融合为一体**，每个 slot 自持：

- `space`（`dir/pt/pt2` 页目录与两张用户页表）、`vm`（VMA 单链表）；
- `kstack_top/ksp` 内核栈；`heap_end` + 静态 `heap_free_root[slot]`；
- 退出码、`parent`、`pgid`、FPU 保存区、ls 快照、管道/日志重定向字段。

调度：`tasks[16]` 静态表，`pick_ready()` 轮询 `T_READY`；`schedule_to()` 切换时
` tss_set_kstack(next->kstack_top)` + `cpu_cr3_write(next->space.dir)`，FPU 懒切换。

内核栈：`static uint8_t task_kstacks[MAX_TASKS-1][8192]`（既有静态池，红线内保留）；
idle 用独立 `idle_stack[4096]`。**帧池 `frame_alloc()` 只给单 4 KiB 帧，没有
>4 KiB 连续分配器，也没有通用内核堆**——这是每线程内核栈方案的硬约束。

## 目标设计（早期设计草图；实际落地口径以首节为准）

### 进程 / 线程所有权拆分

- 引入进程持有者概念：主线程（main thread）持 `space`、`vm`、堆根、管道/控制台/
  日志归属；工作线程挂到所属进程，**共享** `space.dir/pt/pt2` 与 `vm` 链表，自带：
  - 独立 slot（复用既有静态内核栈池 `task_kstacks[slot-1]`，不新增静态栈数组）；
  - 独立用户栈（在扩展 arena 匿名区按 VMA 分配，带 guard 页，懒装入）；
  - 独立 join 状态（`detached` / `thread_ret` / 等待者）。
- 退出拆分：
  - **工作线程退出**：只回收自身用户栈 VMA、TLS 帧、FPU 归属；**不**销毁共享
    `space`、不释放共享 VMA 节点、不动共享堆/管道/控制台；分离线程自动回收，
    否则留 zombie 等 join 回收。
  - **主线程退出**：现有 `task_cleanup` 全量销毁 `space`/VMA/堆/管道/控制台，并
    先回收其余工作线程。
- 堆：线程共享进程堆；`heap_free_root` 需从 per-slot 改为 per-process（本轮探针
  不在线程内 `alloc`，共享堆分配器改造列入后续）。

### 用户区布局（不抬基址）

```
0x2000000 .. 0x2400000  PDE 8：镜像 / vsyscall / heap / 主线程栈（既有）
0x2400000 .. 0x2700000  PDE 9 扩展 arena：匿名 mmap + 工作线程栈（懒装入，guard 页）
0x2700000 .. 0x2780000  Stage 2A 库区（既有，未动）
0x2780000 .. 0x2800000  保留余量
```

线程栈上限（待实测标定）：扩展 arena 匿名区扣除既有 mmap/库后，建议单线程栈
32 KiB、并发工作线程 ≤4（与 16 slot 上限、既有 shell/进程槽位一起核算）。

### TLS / errno / canary

- **已落地（3A-3）**：per-thread TLS 走 `IA32_GS_BASE` MSR，TLS 为独立用户映射页
  （非 GDT TLS 段 / LDT）。内核与 CE 均 `-fno-stack-protector`，canary 暂不参与；
  TLS `[8]=0xC0FFEE` 仅作占位校验。
- **剩余缺口**：CE 用户态尚无 `__tls_errno` 取址 / `mov %gs:off` 直读访问器，探针
  `errno=0` 为占位；errno 词的用户态读写 deferred 到 3B。

## 3B：静态 scratch 改 per-thread（可降级）

摸底现状：

- `src/dl.c`：暂存帧 `meta`/`st` 已在 `dl_open` 内按需 `kzalloc_frame`、返回前
  `frame_free`（Stage 2A 重构），**无大静态暂存缓冲**；静态表仅 `dl_mods[16]`
  描述符与 `dl_next_handle`，句柄按 tid 索引。多线程下库句柄表的归属/加锁是后续项。
- `src/netstack.c:623`：`static uint8_t pseudo[12+8+2048]` 静态暂存，注释明确
  依赖"内核非抢占所以共享 scratch 安全"。
- `src/ssh.c`：会话/收发静态缓冲（待逐函数清点）。
- 未发现名为 `g_in_syscall` 的全局嵌套计数；syscall 嵌套/重入保护目前隐含在
  单核非抢占模型里。

**deferred**：netstack/ssh 静态 scratch 与未来 syscall 嵌套计数改 per-thread；
`dl_mods` 表的多线程归属与互斥。

## 3C：内核态可抢占（最可能 deferred）

**当前内核态仍非抢占**：PIT ISR 只抢占 ring3 用户态任务；ring0 内核代码在阻塞点
（键盘/网络/磁盘）协作式让出，没有定时器抢占点，也没有关抢占区计数。

依赖"内核非抢占"这一前提、3C 落地前不得假设可重入的模块：

- `src/netstack.c`（静态 `pseudo` 暂存、协议栈收发状态）；
- `src/ssh.c`（会话状态机与收发缓冲）；
- `src/dl.c`（库描述符表、暂存帧生命周期）；
- 任务/管道/控制台/日志/ls 快照等 per-slot 静态缓冲（`task.c` 内 `ls_buf`、
  `e3_*_buf` 等按 tid 索引的静态区）。

**deferred**：定时器抢占点、`preempt_disable/enable` 关抢占区、抢占计数与栈深度
安全审计。

## 验收探针（3A 已实机跑通；共享 text 证据属 2C 遗留）

- 4 线程探针（`extapp/threads.c`，已 PASS）：各自栈数组互不干扰；join 返回值正确
  （求和 10）；detach 后退出不崩；杀一线程非零退出不连坐进程空间。
- 共享证据：第二个程序加载同一库/代码段时 text 物理页不重复装入（打印物理页 /
  shared 计数）。Stage 2A 当前形态是每任务私有帧副本，text 尚未跨任务共享（2C 遗留）。

## 下一步

1. **3A-3 / 3A-4（已落地，见首节）**：槽 83–86 真分发、共享 space、MSR TLS、
   四线程探针 `THREAD PASS`；三门禁 vprobe 10/10、E3 2/2、F11 9/9。
2. **3B**：netstack/ssh 静态 scratch 与 dl 句柄表归属迁移 per-thread；共享堆分配器
   per-process；detached 内核栈回收；TLS errno CE 访问器；按 tid 索引静态缓冲串扰。
3. **3C**：内核态抢占点 + 关抢占区，先解除 netstack/ssh/dl 的非抢占假设。
4. 全绿后版本号才走 v2.81；不动 v2.80 ISO/版本串。
