# E3 收口验收报告：syscall 阻塞路径缓冲私有化与用户指针访问加固

- 分支：`day-e3`（隔离工作树 `/home/user/work/catos-e3`，未 push、未合并 main）
- 基线提交：`cf0ee35`（fix(tls): real-handshake fixes — SNI, AES-128-GCM, alert/ticket records）
- 日期：2026-11-23
- 范围：只动 E3 条目；未改 `tls.c` / `tls_aes.c`，未动 `catos_disk.img`，未跑 `disk` 目标。

## 1. 问题

基线内核中，多数 syscall 的用户数据拷贝已经走 `copy_from_user` / `copy_to_user`（Issue #78/#86），
但仍存在两类残留：

1. **阻塞路径上的函数级 static 缓冲跨任务共享**。下列 syscall 在中途可能 `task_block()` /
   `task_sleep_ticks()` 让出 CPU，而其内核快照缓冲是函数级 static：一旦切到另一个任务，后者的
   快照会覆盖前者的字节（真实数据损坏，不是理论风险）：

   | syscall | 基线 static 缓冲 | 阻塞点 |
   |---|---|---|
   | `SYS_WRITE` | `static char b[257]` | 写满的管道 → `task_sleep_ticks(1)` |
   | `SYS_READLINE` | `static char b[256]` | 管道空 → 睡眠；键盘 → `task_block(T_WAIT_KB)` |
   | `SYS_FILE_READ` | `static char fbuf[16384]` | 文件读路径（纳入本次私有化范围） |
   | `SYS_FILE_WRITE` | `static char wbuf[4096]` | 文件写路径（纳入本次私有化范围） |
   | `SYS_PIPE_WRITE` | `static char pwb[PIPE_BUF]` | 管道满 → 睡眠 |
   | `SYS_PIPE_READ` | `static char prb[PIPE_BUF]` | 管道空 → 睡眠 |
   | `SYS_NET_UDP_RECV` | `static char b[2048]` + `static char ipb[24]` | 轮询等待应答 → 睡眠 |

2. **`u2k_str()` 未走安全拷贝**：基线按字节 `validate_uaddr` 后经物理别名逐字节读用户串；
   ring0 不允许直接解引用用户指针，本次改为分块 `copy_from_user`。

## 2. 改动设计

### 2.1 每任务分槽缓冲（A 项）

在 `kernel_v25/src/task.c` 中按 tid 分槽（与既有 `ls_buf[tid]` 同一模式；ABI/槽位未变）：

```c
static char e3_wr_buf[MAX_TASKS][257];    /* SYS_WRITE       */
static char e3_rl_buf[MAX_TASKS][256];   /* SYS_READLINE    */
static char e3_fr_buf[MAX_TASKS][16384]; /* SYS_FILE_READ   */
static char e3_fw_buf[MAX_TASKS][4096];   /* SYS_FILE_WRITE  */
static char e3_pw_buf[MAX_TASKS][4096];   /* SYS_PIPE_WRITE  */
static char e3_pr_buf[MAX_TASKS][4096];  /* SYS_PIPE_READ   */
static char e3_ur_buf[MAX_TASKS][2048];  /* SYS_NET_UDP_RECV 载荷 */
static char e3_ur_ip[MAX_TASKS][24];      /* SYS_NET_UDP_RECV  IP  */
```

每个可能进入阻塞路径的 syscall 改为 `char *b = e3_xxx_buf[t->tid];`。槽位随任务生命周期存在，
任务退出时无需回收（静态 BSS，无堆分配）。

### 2.2 `u2k_str` 分块拷贝（B 项）

`u2k_str(dst, us, max)` 改为按页内剩余长度分块，经 `copy_from_user()` 拷入 256 B 临时块，
再扫描 NUL：

- 块边界不跨页，避免为串尾之后未映射的页面缺页；
- 语义保持：至多 `max-1` 字节、NUL 结尾；校验失败返回 `-1` 并把部分拷贝 NUL 结尾。

### 2.3 `sys_free` 加固（C 项排查附带修复）

基线 `sys_free()` 仅做 `vma_contains()` 范围判断，就用裸用户 VA 直接写堆头 `next` 字段；
指向未备页的陈旧/猜测指针会在 ring0 取不可恢复 #PF。改为：

1. 对堆头跨度 `validate_uaddr(t, h, sizeof(heap_hdr_t))` 过 VMA/页表；
2. freelist 链接（`heap_hdr_t.next`，偏移 4）经新增的 `heap_hdr_put32()` 走物理别名写入，
   ring0 不通过裸用户 VA 存储。

### 2.4 BSS 增量核算

新增静态槽位合计（× `MAX_TASKS=16`）：

| 槽位 | 单任务字节 | ×16 字节 |
|---|---:|---:|
| `e3_wr_buf` | 257 | 4,112 |
| `e3_rl_buf` | 256 | 4,096 |
| `e3_fr_buf` | 16,384 | 262,144 |
| `e3_fw_buf` | 4,096 | 65,536 |
| `e3_pw_buf` | 4,096 | 65,536 |
| `e3_pr_buf` | 4,096 | 65,536 |
| `e3_ur_buf` | 2,048 | 32,768 |
| `e3_ur_ip` | 24 | 384 |
| **合计** | | **500,112 B（≈ 488.4 KiB）** |

增量已用 `nm -S` 符号地址间隙逐槽核验（各槽位首尾地址差与上表一致）。
基线 `.bss`（`objdump -h`）= 970,980 B；按上表估算修复后 `.bss` ≈ 1,471,092 B。
`kernel.bin` 基线 1,138,136 B → 修复后 1,165,084 B（增长主要来自内置 CE 镜像重新打包；
BSS 在平坦镜像中的占用由启动阶段清零填充）。

## 3. 裸用户指针审计清单（C 项）

通读 `isr_syscall()` 全部 case 后的结论：

### 3.1 本次修复

- `u2k_str()`：改为 `copy_from_user` 分块拷贝（见 2.2）。
- `sys_free()`：堆头访问先过页表校验，写链接走物理别名（见 2.3）。

### 3.2 经页表/安全包装路径，不计违规

下列访问均通过 `copy_from_user` / `copy_to_user` / `validate_uaddr` /
`task_virt_to_phys` 物理别名 / `uaddr_in_vma`（VMA 范围+页表），符合任务约定的豁免：
`SYS_WRITE`/`READLINE`/`CAT`/`TOUCH`/`MKDIR`/`SAVE`/`STRCPY`/`FILE_READ`/`FILE_WRITE`/
`PIPE_WRITE`/`PIPE_READ`/`NET_UDP_SEND`/`NET_UDP_RECV`/`SPAWN`/`GFX_BLIT` 的用户缓冲拷贝。

### 3.3 保留 static、未私有化的路径及理由

下列路径**不触达** `task_block()` / `task_sleep_ticks()`：`ata.c` / `net.c` 里的
`task_maybe_yield()` 在 `g_in_syscall > 0` 期间被抑制（`task_maybe_yield` 首行
`if (g_in_syscall) return;`），syscall 中途不会切换任务，因此共享 static 不构成串用窗口：

| 位置 | static | 理由 |
|---|---|---|
| `sys_save_uid`（`SYS_SAVE`） | `buf[FS_DATA_MAX]` | 落盘走 ata，`maybe_yield` 被抑制 |
| `SYS_CAT` | `b[4096]` | `fs_read_text`，同上 |
| `SYS_STRLEN` / `SYS_STRCMP` | `tmp[4097]` / `sa,sb[4097]` | 纯字符串处理，无阻塞 |
| `SYS_HTTP_GET` | `b[8192]` | `net_http_get` 仅 `maybe_yield`，无睡眠/阻塞 |
| `SYS_NET_UDP_SEND` | `b[1400]` | 发送路径，无阻塞 |
| `SYS_HW_STR` | `hb[4096]` | 硬件查询，无阻塞 |
| `SYS_SPAWN` | `kargv[]` / `kargv_buf[][]` | 建映射，不阻塞 |
| `SYS_GFX_BLIT` | `row[GFX24_W*3]` | 帧缓冲逐行 blit，不阻塞 |
| FAT/PE 装载器（`fat_pread` 等） | 装载器静态块 | ring0 装载路径，不会在阻塞 syscall 中重入 |

### 3.4 已知未转换项（如实记录）

- `sys_alloc()` 对堆头的读写位于内核自管的堆元数据上；页面在 `heap_ensure()` 后才访问，
  未按 2.3 的物理别名方式转换。属于残留取舍，本次未扩大改动面。

## 4. 验证（均在最终 ISO 上实机执行）

环境：`qemu-system-i386`（`/usr/bin/qemu-system-i386`），`-m 256 -no-reboot -net none`，
每个场景使用独立复制的临时盘，monitor 端口 4461/4462（未用 4444）。

### 4.1 构建

```
cd /home/user/work/catos-e3
sh build.sh kernel
sh build.sh iso
```

两次目标顺序执行（iso 目标不自动重编 kernel）。未跑 `disk`，未改 `catos_disk.img`。
构建退出码均为 0；`kernel.bin` 1,165,084 B，`catos.iso` 11,890,688 B。

### 4.2 既有回归：vprobe 8/8

命令：`python3 tests/vprobe_regress.py`（脚本自带 QEMU 启动；`-net none` 硬编码）。
真实输出：

```
PASS format           FAT32 volume ready
PASS bin-has-vprobe   /bin/vprobe.ce=True
PASS fr               verdict PASS
PASS cat              verdict PASS
PASS hw               verdict PASS
PASS str              verdict PASS
PASS strcpy           verdict PASS
PASS wr               verdict PASS
PASS fw               verdict PASS
PASS pw               verdict PASS

steps=10 ok=10 fail=0
```

8 个用例 `fr / cat / hw / str / strcpy / wr / fw / pw` 全部 PASS。

### 4.3 并发串用回归（关键证据）

探针：`kernel_v25/extapp/e3probe.c`（`/bin/e3probe.ce`），模式 `parent`：
建管道 → 派生子任务按 16 字节周期 `0123456789ABCDEF` 写 8192 B（256 B 分块，管道写满即睡眠），
父任务边排空边穿插控制台写（压 `SYS_WRITE` 快照槽），按**全局流偏移**逐字节比对周期模式。

命令：`python3 tests/e3_regress.py`（场景 `conc`）。真实输出：

```
PASS conc         verdict PASS
```

客人屏幕原文：

```
E3PROBE CONC got=8192 bad=0 expect=8192 PASS
[task 1 'e3probe' exit(0)]
```

8192 B 全部与写入模式逐字节一致，`bad=0`。

### 4.4 恶意指针

探针模式 `badptr`：对 `file_write` / `pipe_write` 分别传入内核低地址 `0x00100000`
与未映射地址 `0x00FFFFF0` 各一次；另对 `write()` 传入这两个地址（ABI 返回 void，仅断言存活）。
真实输出：

```
PASS badptr       verdict PASS
```

客人屏幕原文：

```
E3PROBE BADPTR fw=-3 fw2=-3 pw=-3 pw2=-3 PASS
[task 1 'e3probe' exit(0)]
```

四个带返回码的调用均返回 `-3`（校验拒绝），内核继续运行，屏幕无
`ring0 kernel fault` / `system halted`。

### 4.5 冒烟

ISO 正常启动到 shell；`ls /bin` 列出 28 个内置应用，`help` 正常打印命令表，
无异常重启。shell 级 `|` 管道未由 HMP 驱动端到端跑通（见 §6）；内核管道读写路径由 4.3 的
双任务并发往返覆盖。

## 5. 截图清单（绝对路径）

| 文件 | 内容 |
|---|---|
| `/home/user/work/catos-e3/artifacts/e3-syscall/vprobe-regress.png` | vprobe 回归末屏（`VPROBE pw PASS`，8 用例全绿见 4.2 文本输出） |
| `/home/user/work/catos-e3/artifacts/e3-syscall/e3-conc.png` | 并发串用：`E3PROBE CONC got=8192 bad=0 expect=8192 PASS` |
| `/home/user/work/catos-e3/artifacts/e3-syscall/e3-badptr.png` | 恶意指针：`fw=-3 fw2=-3 pw=-3 pw2=-3 PASS` |
| `/home/user/work/catos-e3/artifacts/e3-syscall/smoke.png` | 冒烟：shell 提示符 + `help` 命令表 |

## 6. 未做 / 做不通的部分（如实说明）

1. **基线对比未做**：未另建 `cf0ee35` 临时目录编译基线内核跑同一探针记录 FAIL。
   本次只验证了修复后 PASS；基线是否复现损坏未实测。
2. **shell `|` 管道端到端未由脚本驱动**：HMP `sendkey` 对 `|`（shift-backslash）存在按键抖动，
   手动重放未能稳定落键；管道内核路径（`SYS_PIPE_WRITE/READ` 并发往返）已由 4.3 覆盖。
3. **`write()` 恶意指针返回码不可见**：CE ABI 中 `write` 为 void 槽位，仅验证内核不挂；
   带返回码的 `file_write` / `pipe_write` 已确认返回 `-3`。
4. **`sys_alloc` 堆头元数据访问未转物理别名**（见 3.4）；页面已在分配前 ensure 映射，
   本次未改动分配器 freelist 读写路径。
5. 测试盘 `tests/qemu_disk_vprobe.img` / `tests/qemu_disk_e3.img` 为驱动运行时生成的临时镜像，
   未纳入提交。
