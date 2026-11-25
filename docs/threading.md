# 用户态多线程（Stage 3，feat/hosted）

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

Stage 3 目标是在**单核、不碰 SMP** 的前提下给 CatOS 加用户态线程。本轮按时间盒
纪律只落地 **3A 的 ABI 表面（槽 83..86，append-only 预留）**；共享地址空间的
活线程路径、3B per-thread scratch 迁移、3C 内核态可抢占均**如实 deferred**，原因与
下一步见文末。既有进程路径（spawn / wait / pipe / console / heap / FPU）行为
字节级未改，三门禁基线保持绿色。

红线延续：不动 v2.80 交付 ISO 与版本串；CE ABI append-only；不给应用可写像素出口；
33 fps 不动；不新增大 `.bss` 数组；`check_link_layout.py --strict` RC=0。

## 槽位（append-only，不可重排）

| 槽位 | 成员 | 签名 / 语义 | 状态 |
|---|---|---|---|
| 83 | `thread_create(entry, arg)` | `entry` 为 ring3 函数指针 `void *fn(void *arg)`；`arg` 经新线程用户栈传入。返回 tid>0 / 负 errno | 预留，本轮 `ENOSYS (-8)` |
| 84 | `thread_exit(retval)` | 终止调用线程；`retval` 作为 join 返回值 | 预留，本轮 `ENOSYS (-8)` |
| 85 | `thread_join(tid)` | 阻塞直到目标线程退出，回收其槽位，返回 `retval` | 预留，本轮 `ENOSYS (-8)` |
| 86 | `thread_detach(tid)` | 标记线程分离；退出时自动回收，无需 join | 预留，本轮 `ENOSYS (-8)` |

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

## 目标设计（下一轮落地，未编码）

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

- 本轮**未**接 per-thread `GS.base` TLS 块。32 位 x86 下需在 GDT 段描述符侧布置
  TLS 段；内核与 CE 均已 `-fno-stack-protector`，canary 暂不参与。
- 最小可用形态：线程局部变量天然落在各自用户栈上即隔离；errno 类每线程词先经
  线程栈帧承载，待 GS.base TLS 落地后再迁到独立 TLS 页。

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

## 验收探针（待活路径落地后实机跑）

- 4 线程探针：各自栈变量 / errno 词互不干扰；join 返回值正确；detach 后退出不崩；
  杀一个线程不影响进程地址空间与其他线程。
- 共享证据：第二个程序加载同一库/代码段时 text 物理页不重复装入（打印物理页 /
  shared 计数）。Stage 2A 当前形态是每任务私有帧副本，text 尚未跨任务共享（2C 遗留）。

## 下一步

1. 3A 活路径：进程/线程拆分（`task_t` 加 `proc/is_thread/detached/tls_va/
   thr_stack/thread_ret`），线程 create/exit/join/detach 接线，线程栈 VMA 分配与
   退出局部回收，4 线程隔离探针实机 PASS。
2. 3B：netstack/ssh 静态 scratch 与 dl 句柄表归属迁移 per-thread。
3. 3C：内核态抢占点 + 关抢占区，先解除 netstack/ssh/dl 的非抢占假设。
4. 四项全绿后版本号才走 v2.81；不动 v2.80 ISO/版本串。
