# Locale 最小可用（Stage 3 块 4）

## 已落地（内存表骨架 + 查询 syscall + 探针 round-trip）

| 类别 | 状态 | 表 / 行为 |
|---|---|---|
| LC_CTYPE | stub | 接口预留；UTF-8↔GBK 转换全表 deferred |
| LC_NUMERIC | 落地 | decimal_point `.`、thousands_sep `,`（两 locale 同值） |
| LC_TIME | 落地 | 月份 / 星期缩写表：en `Jan..Dec`/`Sun..Sat`，zh `一月..十二月`/`日..六` |
| LC_MONETARY | 落地 | 货币符号 en `$`、zh `¥` |
| LC_COLLATE | **明确不支持** | `setlocale(LC_COLLATE, ...)` 返回 `-22` |
| LC_ALL | 落地 | 一次性设置所有受支持类别（跳过 COLLATE） |

### ABI 槽（append-only）

- 槽 94 `setlocale(category, name)` → `0` 成功；`-22` 非法类别 / COLLATE 不支持；`-1` 未知 locale 名。
- 槽 95 `locale_query(category, what, buf, len)` → `0` 成功；`-22` 非法类别 / 选择子。固定拷 `sizeof(tmp)=32` 字节到用户缓冲。

选择子 `what`：`LQ_NAME=0`、`LQ_DECIMAL=1`、`LQ_THOUSANDS=2`、`LQ_CURRENCY=3`、`LQ_MONTH+1..12`、`LQ_WDAY+0..6`。

支持的 locale 名：`C`/`POSIX`/`en`/`en_US.UTF-8`/`en_US` → C；`zh`/`zh_CN`/`zh_CN.UTF-8`/`zh.UTF-8` → zh。

### 源码 / 归属

| 模块 | 静态缓冲 | 归属对象 | 分配/释放点 | 大小 |
|---|---|---|---|---|
| `src/locale.c` | `cur[LC_COUNT]` 类别状态 | 内核全局（单例，只读查询，无并发写竞争） | bss 零初始化（默认 C）；无运行期分配/释放 | 6×4 = 24 B |
| `src/locale.c` | `mon_en/mon_zh/wday_en/wday_zh` | rodata 常量表 | 链接期常驻 | 纯常量，不计 bss |

净 bss：`_bss_end` 由 `0x02118a00` → `0x02118a18`（+24 B，即 `cur[]`）。未新增其他 `.bss` 大数组。

### 探针

`extapp/locale.c`（内置 `locale`）：默认 C 校验 `$`/`Jan`/`Sun`；切 `zh_CN.UTF-8` 回读 locale 名、非 ASCII 月份/星期/货币；COLLATE 与未知名必须被拒；切回 C 校验 `Dec`。实测 `LOCALE PASS`。

## Deferred

- GBK ctype 转换全表（UTF-8↔GBK 编解码）。
- `printf("%'d")` 千分位格式化器集成（内核/CE 尚无 libc printf 数字格式化器，NUMERIC 表目前只经查询 syscall 暴露）。
- 完整 `strftime` 指令集（当前仅月份/星期缩写可查）。
- 盘上 mmap 数据表（当前全部内存表）。
- 千分位 grouping 规则、货币 fraction digits / 正负号格式等完整 MONETARY 字段。

## 已知卡点

本机 QEMU 无 PCnet/e1000，联机 curl/SSH 冒烟无法在此环境拉起（沿 3C-1 记录）；locale 路径不依赖网络。

## 第二轮（块1）尝试记录 —— 整体 deferred，未入库

第二轮曾尝试落地 LC_CTYPE 转换、千分位格式化与 `strftime`，编译 / strict 通过但真机
未取证，已全部回滚；WIP 备份在探针 scratch（非 git 跟踪）。现状如下，恢复时据此继续：

- **LC_CTYPE 覆盖范围（草案）**：生成脚本遍历 GB2312 符号区 A1–A9 + 一级汉字区
  B0–D7，共 4470 条；**二级汉字区 D8–F7 不收录**，查表返回 `-1 EILSEQ`。二级缺失
  属明确回退行为，不是静默错码。产物为脚本生成的紧凑双升序表（源码 + 生成后的
  `.inc`），不手写几万项。
- **未落地项与卡点**：
  - GBK↔UTF-8 表 / 槽 96 `gbk_to_utf8`、97 `utf8_to_gbk`：随回滚撤销；恢复前置是
    先核对 CE→内核 syscall 门（int80 / 门入口）到底保存哪些寄存器。
  - `printf("%'d")` 千分位：内核 / CE 原本没有 libc 数字格式化器，CE 数字输出只有
    syscall `print_int`/`print_hex`；格式化器探针首版 #PF（varargs 读格式串崩），改
    显式参数后回到 `ce_main` 的 `sstrcmp` 循环 `movzbl (%ebx,%eax,1)`、`ebx=0`（栈
    缓冲指针丢失）。**未证实方向**：CE→内核 syscall thunk 可能不保留 callee-saved
    `ebx`。定位寄存器保存清单前不重写格式化器。
  - LC_TIME `strftime`（`%Y %m %d %H %M %S %b %a %j %p`）：CE 侧 `ce_strftime`
    草案在备份中，未真机比对；固定时间戳 `2026-11-26 02:43:00` UTC+8 逐条比对的
    探针未跑通。
  - 盘上 mmap 数据表：未做，如实标注 deferred（当前全部内存表）。
