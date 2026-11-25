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
| 77 | `file_mmap(fd, len, prot, flags)` | Stage 2B 预留，本轮 `ENOSYS (-8)` |
| 78 | `mprotect(addr, len, prot)` | Stage 2B 预留，本轮 `ENOSYS (-8)` |
| 79 | `munmap_shared(addr, len)` | Stage 2B 预留，本轮 `ENOSYS (-8)` |
| 80 | `dlopen(path)` → handle / 负 errno | Stage 2A 已接线 |
| 81 | `dlsym(handle, name)` → 函数指针 / NULL | Stage 2A 已接线 |
| 82 | `dlclose(handle)` → 0 / 负 errno | Stage 2A 已接线 |

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

### Deferred（2B：文件映射新槽）

- CE 层目前只有路径式文件访问，**没有 fd 句柄表**；`file_mmap(fd, ...)` 依赖一层
  fd 抽象（打开句柄表、引用计数、文件页缓存）。槽 77/78/79 已 append-only 预留，
  分发本轮统一返回 `ENOSYS (-8)`，不重排既有槽位。
- 待前置：fd 句柄表 → 文件页缓存（共享/私有、可懒装入）→ `mprotect` 改 PTE 权限
  → `munmap_shared` 引用计数卸载。红线不变：不给应用可写像素出口，不新增 `.bss`
  大数组（页表/描述符用现有帧池或按需分配）。

### Deferred（2C：用户态 `ld.so` 与共享文本）

- `.dynsym/.rela` 重定位、GOT/PLT 惰性绑定、`init_array` 调用；
- `.text` 页跨任务只读共享（全局只读帧缓存 + 物理页共享）、共享库引用计数、
  引用计数归零后卸载帧；`.data/.bss` 每进程私有槽（COW 可选）；
- 验收探针：两个 CE 程序共用同一 `libce.so`（一个走 printf 风格输出、一个走
  数学/字符串），符号解析 → 各自调用 → 引用计数归零卸载；第二个程序加载时
  text 物理页未重复装入（打印物理页 / shared 计数）。
- 8 MiB 用户区上限见上节：最多 4 库 / 512 KiB 共享 `.text`；若撞墙先扩 VMA 规划
  并写明新上限，不偷抬基址。

## 限制

- 库镜像按任务独立复制，本轮不共享物理页（2C 才做只读共享）；
- 库内全局变量多任务间彼此隔离（各自副本），内存占用随任务数增长；
- 固定基址要求同程序多库地址互不重叠；
- 只有显式 `dlclose`，无依赖闭包卸载；
- 仍是 CSO1 自有格式，不等同 PE/ELF 导入表语义。
