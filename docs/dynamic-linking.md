# CSO1 动态链接库（v2.47）

## 结论

CatOS 现在支持**运行时动态链接**：CE 程序可以在运行时用 `dlopen()` 装载
一个不在自身镜像中的共享库，用 `dlsym()` 按名字解析导出函数并直接调用，
最后用 `dlclose()` 卸载。这是真正的新代码装载，不是构建期合并。

对应的三个 syscall 以 append-only 方式追加到 `catos_api_t` 槽位 66/67/68，
老的 `.ce` 二进制完全不受影响（它们只会看到不存在的尾部槽位）。

## 设计取舍

完整 PE 动态链接器（导入表/导出表/GOT/PLT/共享页/引用计数）在当前内核模型
下成本过高，见 `docs/pe-dynamic-linking-design.md`。本实现采用的是介于
"离线静态重定位" 与 "完整 ld.so" 之间的轻量方案：

- **固定基址**：每个 CSO1 库在链接时就绑定到 `0x2700000..0x2800000` 内的
  一个页对齐地址，装载地址 == 链接地址，因此**不需要重定位引擎**。
- **平坦映射**：库被映射进调用任务自己的 PDE 9 页表，每个任务持有独立
  的物理帧副本；跨任务共享物理页属于后续路线 2 的增强项。
- **同一套 syscall ABI**：库通过装载时注入的 `__ce_api` 指针调用系统服务，
  和普通 CE 程序完全一样，不引入额外的下层 API。

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
export PATH=/c/msys64/mingw32/bin:$PATH
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

built-in 安装器会把 `/bin/libmath.so` 随内核镜像写入 FAT32 卷；
`build/apps.list` 中的 `dltest` 是完整的演示客户端。

## 实机验证

`tests/dl_probe.py`（QEMU 实机，文本模式）：

- `dlopen` 返回 handle 1，六个导出全部解析成功；
- 库函数通过注入的 `__ce_api` 回调内核并成功输出；
- `add/sub/mul/gcd` 结果分别为 12/2/35/12；
- 未知符号与非法 handle 均安全返回 NULL（无崩溃）；
- `dlclose` 返回 0，连续两次运行 dltest 都 PASS（无模块槽泄漏）。

回归：`tests/qemu_regress.py` 12/12（含 mmaptest / hello-elf），
`tests/v245_4gb_probe.py` 在 3840 MiB 下通过，
`tests/btop_probe.py` 通过。

## 限制

- 库镜像按任务独立复制，不共享物理页；
- 库内全局变量若被多个任务同时改写，彼此隔离（因为是各自副本），但
  内存占用会随任务数增长；
- 固定基址要求同一程序装载的多个库地址互不重叠；
- 不支持运行时卸载依赖闭包（只有显式 `dlclose`）；
- 这仍是 CSO1 自有格式，不等同于 Windows DLL / Linux `.so` 的 PE/ELF
  导入表语义；后者属于 `pe-dynamic-linking-design.md` 中的远期路线。
