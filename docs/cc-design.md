# cc.ce —— CatOS 微型 C 编译器（目标一 · 任务 #19）

> 源码：`kernel_v25/extapp/cc.c`（1129 行）
> 产物：`kernel_v25/extapp/bin/cc.ce`（镜像 26,876 B / bss 236,832 B，均在 CE 上限 256 KiB 内）
> 内置安装：`build/gen_builtin.py` 会把 `cc.ce` 嵌入内核 → 盘上 `/bin/cc.ce`

## 用法

    run /bin/cc.ce /SRC/HELLO.C /BIN/OUT.CE

成功后打印 `OK -> <输出路径>`。镜像再由内核 `task_spawn_ce` 以常规 CE 方式加载：
镜像装载到 `0x02000000`，`bss_size` 清零，API 表指针写入头部 `api_offset` 指定的 4 字节槽。

## 支持的语言子集

| 类别 | 支持 |
|---|---|
| 类型 | `int` / `char` / `void`，指针 `*`，一维数组 `[]` |
| 声明 | 全局变量（常量初值、字符串初值）、局部变量、字符串字面量 |
| 函数 | 参数、递归、前向调用、`return` |
| 语句 | 块、`if/else`、`while`、`for`、`break`、`continue` |
| 表达式 | `= += -= *= /=`（右结合）、`?:`、`\|\| && \| ^ & == != < > <= >= << >>`、`+ - * / %`、一元 `- ! ~ * &`、函数调用、下标 |
| API | `api->write("x")` / `api->print_int(n)` 等 48 个槽位均可直接调用 |

**不支持**（编译器会明确报错并给出 token 位置）：`struct` / `union`、`switch` / `case`、`goto`、`typedef`、浮点、多维数组、可变参数、位域、函数指针。

## 代码生成模型

栈式单累加器，表达式结果恒在 `EAX`：

- 二元运算：`push 左` → 求右值 → `pop ecx` → 运算（`ecx`=左，`eax`=右）
- 调用：左到右求值并压栈，再原地反转成 cdecl 的右到左顺序
- 除法/取模：`xchg eax,ecx` 让除数落到 `ecx`，`cdq` + `idiv ecx`
- 局部变量：`ebp` 相对寻址；函数序为 `push ebp / mov ebp,esp / sub esp,frame`

## 镜像布局

```
偏移 0            code（ce_main 位于头部 entry 字段指向处）
偏移 len(code)    data（字符串字面量 + 全局变量）
偏移 len(code)+len(data)   __ce_api 槽（4 字节，内核写入表指针）
```

**槽位放在最后**是有意为之：早期版本把槽放在偏移 4，内核写入表指针时会覆盖
代码第 5 个字节，导致程序一启动就跳到错误地址。

函数调用与数据地址在 `fixup_code()` 里统一回填；由于目标与补丁点同在 code 区，
API 槽的布局偏移在 rel32 计算中相互抵消。

## 构建

    cd kernel_v25/extapp
    sh build_ce.sh cc.c

构建脚本对 CE 程序做了两处关键处理（缺一则 `cc.ce` 无法生成）：

- `--disable-reloc-section`：否则 PE 的 `.reloc` 段落在 `.bss` 之后，`objcopy -O binary`
  会把 851 KiB 的零间隙写进镜像，远超 `CE_MAX_IMAGE`
- `objcopy -R .bss`：`.bss` 由内核按 `bss_size` 清零，不应进入文件

## 验证

    python tests/test_cc.py

宿主侧把 `cc.c` 编译成本机可执行（`tests/host_shim.c` 用真实文件系统实现 `catos_file_*`），
再把产出的 `.ce` 交给 `tests/ce_sim.py` 执行并比对输出。当前 8/8 用例通过：

算术、除法/取模、`for`+`break`、`while`、递归（fib(10)=55）、全局 `char *` 字符串、比较运算。

> QEMU 实机复测因本机无 QEMU 且下载受阻，留待下一轮。
