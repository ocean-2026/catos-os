# CSO1 动态链接库（Stage 2A，feat/hosted）

## 结论

CatOS 在 `feat/hosted` 分支落地了 **Stage 2A：构建期预链接固定基址共享库**。
CE 程序运行时用 `dlopen()` 装载一个不在自身镜像中的共享库，`dlsym()` 按名字
解析导出函数并直接调用，最后 `dlclose()` 卸载。这是真正的新代码装载，不是
构建期合并；装载地址 == 链接地址，**零运行期重定位**。

三个库 syscall 以 append-only 方式追加到 `catos_api_t` 尾部，老 `.ce` 二进制
完全不受影响（它们只会看到不存在的尾部槽位）。

## 槽位（append-only，不可重排）

| 槽位 | 成员 | 状态 |
|---|---|---|
| 77 | `file_mmap(fd, len, prot, flags)` | Stage 2B 已接线：只读文件映射，返回用户 VA / 负 errno |
| 78 | `mprotect(addr, len, prot)` | Stage 2B 已接线：仅允许降权（RW→RO），升权 `EPERM (-11)` |
| 79 | `munmap_shared(addr, len)` | Stage 2B 已接线：减缓存引用，归零释放帧 |
| 80 | `dlopen(path)` → handle / 负 errno | Stage 2A 已接线 |
| 81 | `dlsym(handle, name)` → 函数指针 / NULL | Stage 2A 已接线 |
| 82 | `dlclose(handle)` → 0 / 负 errno | Stage 2A 已接线 |
| 87 | `fd_open(path, flags)` → fd ≥ 3 / 负 errno | Stage 2B 已接线（只读打开） |
| 88 | `fd_close(fd)` → 0 / 负 errno | Stage 2B 已接线：回收句柄槽 |
| 89 | `fd_read(fd, buf, len)` → 字节数 / 0 EOF / 负 errno | Stage 2B 已接线 |
| 90 | `fd_write(fd, buf, len)` → 字节数 / 负 errno | Stage 2B 占位：只读打开，写返回 `EACCES (-13)` |
| 91 | `fd_lseek(fd, off, whence)` → 新偏移 / 负 errno | Stage 2B 已接线（SET/CUR/END） |
| 92 | `fcache_stat(what, arg)` → 计数 | Stage 2B 诊断：0=驻留页数，1=累计命中，2=某 VA 页引用计数 |

## 用户区 VMA 布局（未抬 `USER_IMAGE_BASE` / `CE_LOAD_BASE`）

8 MiB 用户区保持不变，库区在现有 PDE 9 扩展 arena 内顶部划出，匿名 mmap / PIE
首配扫描被限制在库区之下：

```
0x2000000 .. 0x2400000  PDE 8：CE 镜像 / vsyscall / heap / stack（既有，未动）
0x2400000 .. 0x2700000  PDE 9：匿名 mmap + PIE 镜像（3 MiB，懒映射）
0x2700000 .. 0x2780000  PDE 9：CSO1 固定基址库区（512 KiB）
0x2780000 .. 0x2800000  PDE 9：保留余量（未使用）
```

### 库区上限

- 单库 span = image(≤64 KiB) + bss(≤64 KiB)，按页向上取整（≤128 KiB）；
- 512 KiB 库区最多容纳 **4 个**并发库（`MAX_DL_MODULES = 16` 是描述符表上限，
  实际受库区帧容量约束为 4）；
- 总共享 `.text` 上限即 512 KiB；同程序内多库基址必须页对齐、互不重叠。

## 设计取舍

完整 PE/ELF 动态链接器（导入表/导出表/GOT/PLT/共享页/引用计数）在当前内核模型
下成本过高，见 `docs/pe-dynamic-linking-design.md`。本阶段采用介于"离线静态重定位"
与"完整 ld.so"之间的轻量方案：

- **固定基址**：每个 CSO1 库在链接时绑定到库区内一个页对齐地址，装载地址 ==
  链接地址，不需要重定位引擎。
- **平坦映射（本轮形态）**：库被映射进调用任务自己的 PDE 9 页表，每个任务持有
  独立物理帧副本；`.text` 跨任务只读共享 + 引用计数是 Stage 2C 增强项（见下）。
- **每进程私有数据**：`.data/.bss` 随帧每进程私有一份（固定偏移数据槽），本轮
  未做 COW。
- **同一套 syscall ABI**：库通过装载时注入的 `__ce_api` 指针调用系统服务，和
  普通 CE 程序一致，不引入额外下层 API。
- **.bss 红线**：内核侧不新增大静态数组；文件分块经一个按需分配、用完即释放的
  暂存帧读入，导出表 + 名字 blob 落在一个内核元数据帧里。

## CSO1 文件格式

```
偏移  长度  字段
0     4     magic "CSO1"
4     4     version = 1
8     4     load base VA（页对齐，落在库区内）
12    4     image_size（代码+数据原始字节）
16    4     bss_size
20    4     api_offset（4 字节 API 指针槽偏移；0xFFFFFFFF = 无）
24    4     export_off（导出表在文件中的偏移）
28    4     export_cnt
...   8*cnt 导出项 { name_off, func_off }
...   N     NUL 分隔的导出名
...   image_size 原始镜像
```

## 使用方式

构建共享库（每个库使用不同的页对齐基址）：

```sh
cd kernel_v25
sh extapp/build_so.sh libmath.c bin 0x2700000 \
    add=lib_add,sub=lib_sub,mul=lib_mul,gcd=lib_gcd,version=lib_version,hello=lib_hello
```

CE 程序侧：

```c
#include "ce_abi.h"
int h = api->dlopen("/bin/libmath.so");
int (*add)(int,int) = (int(*)(int,int))api->dlsym(h, "add");
int r = add(7, 5);              /* 12 */
api->dlclose(h);
```

`build/apps.list` 中的 `dltest` 是完整演示客户端；内置安装器把 `/bin/libmath.so`
随内核镜像写入 FAT32 卷。

## 实机验证（Stage 2A 探针）

QEMU 实机（文本模式）运行 `dltest.ce`：

- `dlopen` 返回 handle 1，六个导出全部解析成功；
- `version` 输出 `libmath 1.0 (CSO1 shared object)`；
- `hello` 经注入的 `__ce_api->write` 回调内核输出
  `[libmath] hello from a shared object`（printf 风格出口）；
- `add/sub/mul/gcd` 结果分别为 12 / 2 / 35 / 12（数学/字符串出口）；
- 未知符号与非法 handle 安全返回 NULL（无崩溃）；
- `dlclose` 返回 0，`dltest: PASS [task 1 'dltest' exit(0)]`，无 #PF/#GP/#UD。

## 进度与 deferred

### 本轮已落地（2A）

- 库区常量与 VMA 类型 `VMA_LIB`；匿名扫描/卸载范围收到库区之下；
- `mkso.py` 封包器（补回此前缺失的 CSO1 打包脚本）；
- 内核 `dl.c` 接入构建与 syscall 分发；`task_cleanup` 回收库元数据帧；
- 示例 `libmath.so` 与演示客户端 `dltest.ce` 内置到 `/bin`。

### 本轮已落地（2B：fd 句柄表 + 只读文件页缓存 + 槽 77/78/79 真实现）

**fd 句柄表（每进程一份，槽 87–91）**

- 每任务固定一张 `fdtab[MAX_TASKS][MAX_OPEN_FILES]`，`MAX_OPEN_FILES = 8`；
  用户 fd 从 `FD_FIRST = 3` 起，`0/1/2` 保留 stdin/stdout/stderr。
- `fd_open(path, flags)`：`fs_resolve_file` 解析首簇+大小，`PERM_READ` ACL 校验，
  分配空闲槽；本轮一律按**只读**打开（`flags` 暂未解析）。
- `fd_close(fd)`：置槽空闲、回收；非法 fd 返回 `EBADF (-22)`。
- `fd_read(fd, buf, len)`：从当前偏移读，推进偏移；`fd_lseek` 支持 SET/CUR/END，
  越界裁剪到文件大小。`fd_write` 为占位：只读打开下写返回 `EACCES (-13)`，
  偏移写（offset-write）未实现。
- **路径式既有接口完全保留兼容**：槽 39 `file_read(path,...)`、40 `file_write`、
  41 `file_delete` 一行未改，老 CE 程序零改动可用。

**最小只读文件页缓存（全局固定池）**

- 池 `cache[FCACHE_MAX_PAGES]`，`FCACHE_MAX_PAGES = 64`，即 **64 × 4 KiB = 256 KiB
  帧预算**；键 =（首簇号, 块号），每页带引用计数 `refcnt`。
- 帧**按需分配**（`kzalloc_frame`），不设大静态暂存；某页引用计数归零即
  `frame_free` 并卸载。任务退出由 `fcache_cleanup_task` 先摘除本任务文件 PTE 并
  减引用，避免共享帧被任务空间销毁路径误释放（新增 `task_scrub_page`：只清 PTE
  不释放帧）。
- **淘汰策略：硬上限，不做 LRU。** 未命中且池满时该次 mmap 直接失败 `ENOMEM (-12)`，
  不淘汰既有页（本阶段文件均小于单页，LRU 收益不足；如需换页再在 2C 评估）。

**映射区布局与权限**

- 文件映射复用扩展 arena 的首配扫描，区间与匿名 mmap 同一片间隙
  `0x2400000 .. 0x2700000`（`USER_EXT_BASE .. USER_LIB_BASE`），CSO1 库区
  `0x2700000 .. 0x2780000` 排除在外；新 VMA kind `VMA_MMAP`。缺页路径按
  `VMA_MMAP` 分支回调 `fcache_fault_page` 取缓存帧，以**只读** PTE 映射。
- 映射只读（`PROT_READ`）。`PROT_WRITE` 请求直接返回 `EACCES (-13)`——本轮**不做
  写时复制**，不复制私有可写帧。`mprotect` 只允许降权（RW→RO），升权
  RO→RW 返回 `EPERM (-11)`。`munmap_shared` 递减引用，归零释放。
- 错误码沿用既有约定：`ENOENT -1 / EINVAL -3 / FULL -4 / ENOMEM -12 /
  EACCES -13 / EPERM -11 / EBADF -22 / ENOSYS -8`。
- vsyscall thunk 页布局：前段 thunk 占 `0x000..0x7FF`；本段填满后，87 起的新 thunk
  落到固定 API 表（`0x800`，`CATOS_VS_TABLE` 常量不可挪）之后、ls thunk `0xC00`
  之前的空闲间隙；守卫分别校验前段 `< 0x800`、溢出段 `< 0xC00`。

**实机探针 `mmap.ce`（`/bin/mmap.ce`，源码 `extapp/mmap.c`）**

流程：`fd_open("/bin/hello.ce")` → `fd_lseek` 取大小 → 路径式 `file_read`
留参照 → `file_mmap`（m1）后逐字节比对；第二次映射（m2）同样触碰首页，证明
两个视图落到同一缓存帧；经槽 92 读取 `cache_hit / pages / refcnt`；`munmap_shared`
摘掉 m2 后再次重映射（m3），仍命中 m1 持有的驻留页。

实机串口取证（`gate-fdmmap-smoke-hosted.log`）：

```
ok   fd_open fd=3
ok   file size=312
ok   path read bytes=312
ok   mmap byte-match chunk0
ok   second mmap (shared cache)
info cache_hit=1 pages=1 refcnt=2
ok   remap after munmap refcnt=2
err  bad-fd=-22
err  prot-write=-13
err  capacity=-4
MMAP PASS
```

- 映射为**惰性装入**：`file_mmap` 只登记 `VMA_MMAP` VMA 与映射描述符（记录首簇、
  文件大小、路径、属主 tid、只读），不在 mmap 时预钉帧；首次访问触发缺页，由
  `VMA_MMAP` 分支回调 `fcache_fault_page` 从缓存取帧并以只读 PTE 映射。
- 错误路径三条均实机取证：非法 fd → `EBADF -22`；`PROT_WRITE` → `EACCES -13`；
  超容量 → `-4`（活跃映射描述符表 `MAX_FILE_MAPS=16` 占满，内核全局表）。
- 任务退出：`task_cleanup` 在 `task_space_destroy` 之前调 `fcache_cleanup_task`，
  摘除本任务全部 file-mmap VMA、清 PTE、减缓存引用，并回收本任务 fd 表，不泄漏。

### 2B 残留 deferred / 降级项

- `fd_write` 偏移写、读写打开模式未实现（当前只读，写返回 `EACCES`）；
- 页缓存无 LRU 淘汰，池满即 `ENOMEM (-12)`。**缓存池满（-12）分支在本镜像上
  不可达**：内置最大文件 < 87 KiB（≈22 页），无法凑满 64 页缓存预算；探针的超容量
  检查实际命中的是活跃映射描述符表上限 `-4`。缓存池满路径仅经代码走查确认；
- 多页/大文件映射、跨任务只读物理页共享属 2C 范畴。

### Deferred（2C：用户态 `ld.so` 与共享文本）

- `.dynsym/.rela` 重定位、GOT/PLT 惰性绑定、`init_array` 调用；
- `.text` 页跨任务只读共享（全局只读帧缓存 + 物理页共享）、共享库引用计数、
  引用计数归零后卸载帧；`.data/.bss` 每进程私有槽（COW 可选）；
- 验收探针：两个 CE 程序共用同一 `libce.so`（一个走 printf 风格输出、一个走
  数学/字符串），符号解析 → 各自调用 → 引用计数归零卸载；第二个程序加载时
  text 物理页未重复装入（打印物理页 / shared 计数）。
- 8 MiB 用户区上限见上节：最多 4 库 / 512 KiB 共享 `.text`；若撞墙先扩 VMA 规划
  并写明新上限，不偷抬基址。

## Stage 2C block ①②：内核 relocate 引擎 + CSO2 rela 封包

### 支持的 R_386 重定位类型

| type | 名称 | 语义 | 槽内写入 |
|---|---|---|---|
| 1 | `R_386_32` | 绝对地址（数据/函数指针表） | `*slot = sym_val + addend` |
| 2 | `R_386_PC32` | 相对调用 / 跳转 | `*slot = sym_val - loc_va - 4 + addend` |
| 6 | `R_386_GLOB_DAT` | GOT 数据项（对象/变量引用） | `*slot = sym_val + addend` |
| 7 | `R_386_JMP_SLOT` | PLT 函数槽 | `*slot = sym_val + addend` |
| 8 | `R_386_RELATIVE` | 基址相对修正（本地符号指针） | `*slot = base + 槽内旧值` |

类型 8 为裁决批准放行：PIC `-shared` 库必然 emit 基址相对修正，不支持
就无法装入真 PIC 库。其余类型由 `dl_reloc_apply()` 显式拒绝，返回
`DL_ERR_RELOC = -8`（ENOEXEC 语义：镜像含内核不支持的重定位），并向 COM1
打点 `dl reloc reject type=<hex>`；**不静默跳过**任何槽。

### 符号与导出条目语义

- 导出条目由「函数偏移」扩成**通用符号偏移**（函数/变量一致）：`dlsym`
  返回的指针统一为 `base + sym_off`；槽位 ABI（80/81/82 入参出参）未动。
- CSO2 rela 的 `sym_idx` **1-based**：0 表示空符号（RELATIVE/无符号），
  导出表下标 = `sym_idx - 1`。这是为了与 ELF 惯例「符号表 0 号为空」对齐，
  避免把首号导出误判成空符号。

### 引擎复用（同一条 relocate 路径）

- **CSO2 `dlopen` 路径**：拷镜像、注入 vsyscall 表指针后，遍历 CSO2 rela
  表（12 B/条 `{r_offset, r_info, r_addend}`）；`r_info` 低 16 位为 type、
  高 16 位为 1-based sym_idx；符号按导出表解析（`sym_val = base + sym_off`），
  逐条过引擎。`R_386_RELATIVE` 的 addend 取槽内旧值，其余符号类 addend=0。
  槽偏移越界、sym 越界同样拒绝。每条成功补丁打点
  `reloc: type=<t> sym=<name> patched=0x<va>`，RELATIVE 打
  `reloc: type=8 slot=0x… base=0x…`。
- **ELF 可执行路径（`spawn_load_elf`）**：节头扫描 `.rel.dyn` / `.rel.plt`
  （`SHT_REL=9`）与 `.dynsym`（`SHT_DYNSYM=11`）；REL 条目 8 B
  `{r_offset, r_info}`；解析后同样调 `dl_reloc_apply()`，拒绝返回 `-8`。
  当前内置 `hello.elf` 无动态节，走节表缺失的 no-op 早返回。

### 封包器口径（mkso.py，主机无 pyelftools）

- 库由 `gcc -m32 -ffreestanding -fPIC` 编译、`ld -m elf_i386 -shared` 链接
  到 vaddr 0；`objcopy -O binary` 出平镜像（文件偏移 == 链接 vaddr）。
- `readelf -r` 文本解析 `.rel.dyn/.rel.plt`：列 `Offset Info Type
  Sym.Value Sym.Name`；类型名映射到上表码。`nm`（全 symtab，非 `-D`）取
  导出符号定义偏移——动态表对可 preemptible 函数可能报 st_value 0。
- 符号名带 `@…` 版本后缀时截到 `@` 前；`nm` 交叉校验缺失符号。reloc
  目标符号不在导出表（除内部保留的 `__ce_api` 槽）即 fail fast。
- `__ce_api` 作为隐藏内部条目挂在导出表末尾（无名），供其 GLOB_DAT 解析，
  不被 `dlsym` 暴露。

### init_array / fini_array

`dl_open` 在重定位完成后按顺序调用 `init_array`；`dl_close` 在解映射前调用
`fini_array`。每条 fnptr 先校验落在模块 span 内，越界跳过并打点。当前内置
库数组为空（cnt=0），调用路径走 no-op。

### 块②实机证据（libmath.so / dltest.ce）

`readelf -r` 含 `R_386_GLOB_DAT`（`lib_seed`、`__ce_api`）与
`R_386_JUMP_SLOT`（`lib_add`）；运行期串口打点：

```
reloc: type=1 sym=add  patched=0x2700404
reloc: type=1 sym=sub  patched=0x2700408
reloc: type=6 sym=seed patched=0x27004a4
reloc: type=6 sym=     patched=0x27004a8
reloc: type=7 sym=add  patched=0x27004b8
```

`dltest` 输出：`seed=7 combo(5)=12 dispatch(0,7,5)=12 dispatch(1,7,5)=2
dispatch(2,7,5)=35`，`dltest: PASS`。`combo()` 经 PLT 调 `lib_add`、读导出
变量 `seed`；`dispatch()` 走重定位后的只读函数指针表——证明 PLT/GOT 补丁
真生效，不是直接调用或只拷镜像。

### 本块未覆盖

- 本库链接器对本地函数指针选择了 `R_386_32` 而非 `R_386_RELATIVE(8)`，
  type 8 引擎分支已实现但本轮未被实机触发；`R_386_PC32(2)` 由库内直接
  相对调用覆盖（链接期已解析，不进 rela 表）。
- `hello.ce` 回归由 ③④ 三门禁覆盖。

## 限制

- 库镜像按任务独立复制，本轮不共享物理页（2C 才做只读共享）；
- 库内全局变量多任务间彼此隔离（各自副本），内存占用随任务数增长；
- 固定基址要求同程序多库地址互不重叠；
- 只有显式 `dlclose`，无依赖闭包卸载；
- 仍是 CSO1 自有格式，不等同 PE/ELF 导入表语义。

## 2C-3 / 2C-4 实现补充（2026-11-25）

- **共享键**：`(cluster, block)`，**不含 pid/tid**；同一键复用同一物理帧，只读映射进各任务 PDE9（PTE `RW=0`，装库期临时可写、装完翻只读）。
- **切段规则**：`[0, text_end)` 共享只读；`[text_end, data_end)` 每进程私有副本，且二次装入时私有段**单独重跑重定位**；跨页边界不对齐的页归**私有**。
- **引用计数**：首个任务装入建池并 refcnt=1，后续任务 +1；末次 `dlclose` 或进程被 `task_cleanup` 回收时减引用，归零则逐页释放并打印 `pages/refcnt` 序列。
- **RELATIVE(8)**：引擎支持 `*slot = base + 槽内旧值`；本工具链的自然产物未被链接器 emit，故用**合成 rela 表**取证（`rel8.so`/`rel8test.ce`，日志 `gate-dynlink2-relative.log`，命中 `slot02710118`）。
- **双进程证据**：`share_parent` 用 spawn + 两根 pipe（ready 子→父、release 父→子）保证两进程同时驻留；A 建池帧 `pa02855000`，B 打印 `(shared)` 同帧；close 序列 `refcnt 2->1->0`、`release pages=1`。
- **已知边界**：`wait` 对已自动回收的子进程返回 -1（既有内核回收竞态），探针按串口 refcnt/帧号为准。
