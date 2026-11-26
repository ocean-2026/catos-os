# P2-F5 · 交叉编译上游源码入仓 —— 设计文档

> 状态：**纯设计文档，本条目尚未实现**。本轮（v2.82 分支 `v282/p1-1-pe-reloc`）
> 只产出本设计，未搭建工具链、未改动任何源码、未新增包。文中「现状」均为
> 本轮在磁盘代码中核实到的事实；「目标态」为后续实施时的方案，不代表已具备。
>
> 关联文档：`docs/pkg-repo-design.md` §2（仓库/工具链/流水线的早期预留设计）、
> `docs/cc-design.md`（guest 内微型编译器 `cc.ce` 的语言子集与镜像模型）。
> 本文是 F5 的落地分解，不重复拷贝上述两份文档的全部细节。

## 1. 现状（磁盘实况）

### 1.1 已在位的 CE 应用构建链路（宿主侧，自研源码）

当前 CatOS 用户态程序以 CE01 扁平镜像形式存在，构建全部发生在**构建宿主**上，
管线由 `kernel_v25/extapp/build_ce.sh` 固定：

1. 宿主 `gcc`（本机 Ubuntu 11.4.0）以 freestanding i386 参数族编译：
   `-m32 -ffreestanding -fno-pic -fno-stack-protector -fno-stack-check
   -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -mno-3dnow -mno-80387
   -fno-asynchronous-unwind-tables -fno-unwind-tables -O2
   -fno-omit-frame-pointer -std=c99 -DCC_HOST`；
2. `ld -m i386pe` 经 `extapp/ce.ld` 链接，入口 `ce_main` 固定在镜像偏移 0；
3. `objcopy -O binary` 剥除 `.bss`（bss 由内核按 `bss_size` 清零）；
4. `extapp/build/mkce.py` 包 CE01 头；运行时桩 `ce_stub.c`（`__chkstk_ms`
   等）与共享层 `ce_app.c` 与每个应用一同链接。

包装产物落在 `kernel_v25/extapp/bin/*.ce`，经 `gen_builtin.py` 嵌入内核镜像，
开机播种到 guest 卷 `/bin/`。

**事实边界（实测/源码核实）：**

- 镜像硬上限 `CE_MAX_IMAGE = 0x40000`（256 KiB，`kernel_v25/include/ce.h`）；
  bss 不计入文件镜像但占运行期零页；CE 任务跑在内核侧已知的 8 KiB 栈上。
- CE ABI 为追加式表 `catos_api_t`（`kernel_v25/include/ce_abi.h`，槽位编号
  以该文件实际定义为准）；新槽只能追加在表尾，三处同序同步
  `ce_abi.h` / `ce.h` / `task.c`。
- 宿主 **没有 multilib C 库头**：freestanding 编译可过，但 CE 源码一旦包含
  主机 `<string.h>` 等 libc 头即报 `bits/libc-header-start.h` 缺失（见 BRIEF
  §1 已修坑②）；现有 CE 程序一律使用内核自带的 `kstring` 族与 ABI 槽，不链主机 libc。
- 现有构建只覆盖 `extapp/`（及 `node/` 等少数目录）内**自研单文件 C**，
  没有上游 tarball 解包、configure/make、第三方源码目录或 sysroot 概念。

### 1.2 已在位的包链路（`pkg`）

- 容器格式 `CPKG02`：204 字节定长头（magic/name/version/cmd/desc/
  payload_len + RSA-512 签名）+ 原始 CE01 负载；宿主侧 `tools/mkpkg.py`
  打包，内核 `pkg_verify` 验签，验签不符拒装。
- `pkg fetch NAME|URL`（`kernel_v25/src/shell.c:cmd_pkg_fetch`）：经 e1000 +
  既有 HTTP 流式下载把**现成的签名 `.catpkg`** 拉到暂存文件，再交
  `pkg_install_file` 验签解包到 `/bin/<cmd>.ce`；下载按 HTTP Range 分片续传
  （单连接在 QEMU slirp 下约 80 KiB 空闲断连，最多 400 跳）。
- 本地索引 `/pkg/PKGS.TXT` 行为 `name|version|desc|file`；当前索引**没有**
  源码来源、依赖、构建标识字段。

### 1.3 缺口（F5 要补的，现状明确不具备）

| # | 缺口 | 现状 |
|---|------|------|
| G1 | 上游源码交叉构建链 | 无：没有 per-package 上游源码目录、没有针对上游 C 的 sysroot/移植桩规范、没有从 tarball 到对象文件的构建脚本族 |
| G2 | `pkg fetch` 不碰源码 | 只下载预编译 `.catpkg`；不解包源码、不在 guest 内或宿主侧触发编译、索引无溯源/依赖字段 |
| G3 | guest 内编译器能力有限 | `cc.ce`（`extapp/cc.c`）是微型栈式编译器，子集见 `cc-design.md`：不支持 `struct/union`、`switch/case`、`goto`、`typedef`、浮点、多维数组、可变参数、位域、函数指针；多数上游 C 代码无法直接被它编译 |
| G4 | 宿主工具链非完整交叉链 | 现有 `gcc -m32 -ffreestanding` 是裸机 freestanding 用法，没有 i386-catos 目标三元组、没有 newlib/托管层、没有调试符号剥离与 sysroot 隔离的固定流程 |
| G5 | 许可证与溯源未登记 | 包索引与容器头均不记录上游出处、许可证、源码指纹；CPKG02 签名只覆盖「宿主上打包好的负载」，不构成源码溯源证明 |
| G6 | 无依赖解析 | 不识别包间依赖；上游程序若需配套库/数据文件，当前无法声明与拓扑安装 |

## 2. 目标与非目标

### 2.1 最小可用目标（F5 本轮设计范围）

打通一条可重复的链路：**上游 C 源码 → 构建宿主侧 freestanding 交叉编译 →
CE01 镜像 → `mkpkg.py` 打 `.catpkg` 入仓 → guest 内现有 `pkg fetch` 拉取、
验签、安装、运行**。其中 guest 侧 `pkg fetch/install/verify` 已在位，F5 新增的
是宿主侧「源码到镜像」这一段及其打包元数据约定。

补充路径（次级，不进最小闭环）：对语言子集恰好落在 `cc.ce` 能力内的源码，
允许 guest 内现场编译生成 `.ce`，免去宿主构建环节。

### 2.2 成功判据（后续实施时验收，本轮不承诺）

- 至少一个上游单文件 C 工具经本链路产出 `.catpkg`，在 QEMU 实机 `pkg fetch`
  安装后可运行并通过对应探针；
- 产物镜像大小、bss 落在 256 KiB 上限内；构建脚本可在干净宿主上复跑；
- 索引条目记录源码出处/许可证/上游版本（字段新增需过 ABI 与索引格式评审）。

## 3. 约束基线（实施时不可违反）

1. **镜像/栈预算**：`.ce` 文件镜像 ≤ 256 KiB；运行栈 8 KiB；大数组/缓冲必须
   走 ABI 分配槽并在审计中核对，不得隐式开大栈帧。
2. **CE ABI 追加式**：若上游能力必须通过新内核槽位暴露，新槽只能加在表尾，
   `ce_abi.h` / `ce.h` / `task.c` 三处同序同步；不为 F5 重排既有槽位。
3. **freestanding**：不链主机 libc、不引主机 libc 头；所有库语义调用先映射到
   ABI 槽或移植桩，桩内不得假设主机运行时存在。
4. **链接护栏**：任何新增静态表/大数组后，`tools/check_link_layout.py
   build/kernel.pe --strict` 必须 RC=0（本设计不新增内核表，此条约束后续改动）。
5. **不回退已修坑**：`mkrel8.py` 保持 tracked；`term.c` 类文件只引 `kstring.h`，
   不引主机 `<string.h>`。

## 4. 最小可用路径（分阶段方案）

> 阶段划分按「先打通一条端到端样例，再扩面」排序；每个阶段给出输入、动作、
> 产出与验收观察点。阶段 A–D 为宿主侧闭环，阶段 E 为 guest 内编译补充路径。

### 阶段 A · 选源与可移植画像

- **选源标准**（硬门槛，先筛后编）：
  - 单文件或极少文件纯 C，C99 及以下；无 C++/汇编强依赖；
  - 不依赖 `struct/union` 重度网络协议栈、浮点、动态加载、线程库；
  - 功能可被现有 ABI 槽覆盖：控制台输出、文件读写、分配、时间/随机等以
    `catos_api_t` 已有槽为准；缺什么能力先记入缺口表，不临时扩 ABI；
  - 源码许可证允许随仓分发（在索引中登记）。
- **产出**：一份候选上游清单（名称/上游 URL/版本/许可证/文件数/预估镜像量/
  依赖的 ABI 槽/阻塞点），作为阶段 B 的输入。
- **观察点**：清单中每个候选都标注「可直接移植 / 需移植桩 / 不达标」，
  不达标项直接列入第 6 节非目标，不硬上。

### 阶段 B · 宿主侧交叉构建 harness

- **目录约定**（建议，实施时定）：`kernel_v25/extapp/ports/<name>/` 放上游
  源码副本与本地移植补丁；构建产物仍走 `extapp/bin/<name>.ce` 管线。
- **复用现有参数族**：直接继承 `build_ce.sh` 的 CFLAGS（见 §1.1），不另起
  一套编译器选项；入口仍要求 `ce_main` 在镜像偏移 0。
- **移植映射层**：为每个 port 写一个薄桩文件，把上游常用 libc 语义翻译到
  ABI 槽 / 内核 `kstring` 族，例如：
  - 字符串：`strlen/strcpy/strcmp` 类 → 内核 `kstring.h` 等价函数；
  - 输入输出：`printf/puts/getchar` 类 → ABI `write/print_int` 等槽；
  - 文件：`fopen/fread/fwrite` 类 → ABI 文件槽（路径、偏移、错误码对齐现有
    guest 文件语义）；
  - 分配：`malloc/free` → ABI 分配槽；不假设主机堆。
  - 桩文件与上游源码同目录编译，随 `.ce` 一起进镜像；桩头单独评审，避免
    把主机 libc 头拽进来。
- **构建脚本**：每 port 一个 `build.sh`（或统一 `ports/build.sh <name>`），
  输入源码目录，输出 `bin/<name>.ce`；脚本内显式校验：镜像大小 ≤ 256 KiB、
  bss 大小在预算内、`ce_main` 位于偏移 0。
- **不做**：不在本轮引入 autotools/cmake 跨平台 configure；上游自带
  Makefile 的，先人工裁剪为上述裸编译命令。

### 阶段 C · 镜像预算与双环境验证

1. **宿主侧仿真**：复用现有 `tests/ce_sim.py` / host shim 路径跑基本用例，
   先在宿主上验证编译产物逻辑与桩映射正确；
2. **QEMU 实机**：`.ce` 注入 guest（参照 `tests/pe_reloc_probe.py` /
   `ce_regress.py` 的两轮磁盘流程），跑最小功能探针，记录屏幕/串口输出与截图；
3. **预算复核**：记录镜像字节数、bss 字节数、栈使用估算；超限即裁剪功能或
   退回阶段 A 重选源，不压缩预算红线。

### 阶段 D · 打包入库与拉取

- 复用 `tools/mkpkg.py`：元数据头填 name/version/cmd/desc，负载为阶段 C
  产出的 `.ce`；私钥仍只在宿主侧，验签走既有 CPKG02 流程。
- 索引条目在现有 `name|version|desc|file` 基础上**预留**溯源字段（上游 URL、
  上游版本、许可证、源码指纹）；字段格式变更需评审索引解析代码（guest 侧
  `pkg_load_index`），保持旧条目向后兼容。
- 仓库布局沿用 `pkg-repo-design.md` §2.1：`pool/` 存源码 tarball，
  `binary-i386/` 存 `.catpkg` 与索引；guest 侧 `pkg fetch` **不需要为
  预编译包链路改代码**——它拉的仍是现成签名包。

### 阶段 E · guest 内现场编译（次级补充，非最小闭环）

- 适用范围：源码语言子集落在 `cc.ce` 能力内（无 struct/float/varargs/…），
  且无外部依赖。
- 链路设想：`pkg fetch` 增加源码包类型（容器 magic 分派，负载为源码文本）→
  guest 内落盘 → 调 `/bin/cc.ce` 现场编出 `.ce` → 再走常规安装。
- 前置缺口：`cc.ce` 目前在 guest 内的文件读写、错误提示、镜像预算控制需先
  补齐；该路径**不得**承诺编译任意上游代码，只服务子集内小程序。

### 4.1 两条路径对照

| 维度 | 路径 A：宿主交叉编译（最小闭环） | 路径 E：guest 内 cc.ce 现场编译 |
|------|----------------------------------|--------------------------------|
| 编译发生地 | 构建宿主 | CatOS guest 内 |
| 语言范围 | 现有 freestanding gcc 可编的 C99，受 ABI/预算约束 | `cc.ce` 子集（见 §1.3 G3） |
| guest 侧改动 | 无（复用现有 pkg 链路） | 需扩源码包类型 + cc.ce 文件/预算能力 |
| 上游可得性 | 宿主需源码与构建环境 | guest 需先拉源码包 |
| 优先级 | F5 最小可用 | 后续补充 |

## 5. 移植映射层的设计约束

- 桩只做「语义翻译」，不新增内核服务；ABI 已有槽覆盖不了的能力，记为缺口
  提交评审，按 append-only 流程扩槽，不在桩里伪造行为。
- 桩代码随 port 目录走，不进内核；多 port 重复用到的桩函数沉淀到
  `extapp/` 共享层（类似现有 `ce_app.c` / `ce_stub.c`），沉淀前先确认不破坏
  既有 CE 应用链接（弱符号/重复 `.ce_api` 槽问题见 `build_ce.sh` 现有处理）。
- 字符串一律走内核 `kstring` 族；禁止在 CE 源码中包含主机 libc 头文件。

## 6. 明确不做的部分（本轮及最小闭环内排除）

1. **不搭建完整 GCC 交叉工具链**：不编 `i386-catos` 目标三元组的 GCC/binutils，
   不移植 newlib/glibc；沿用宿主 `-m32 -ffreestanding` 裸编。
2. **不做托管构建系统**：不在 guest 内或宿主侧引入 make/autotools/cmake 通用
   构建驱动；每 port 一个显式裸编译脚本。
3. **不处理非 C 上游**：C++、Go、Rust、汇编重度程序、解释型运行时（Python/
   JS 引擎等）均不在 F5 范围。
4. **不做动态库/复杂依赖体系**：`.so` 共享库、包间依赖拓扑解析、版本升级/
   回滚沿用 `pkg-repo-design.md` §2.5 的后续条目，F5 不实现。
5. **不改六份用户正文与全局待办总表**；本设计的状态登记由父代理统一回填。
6. **不在本轮实现阶段 E**：guest 内现场编译只留设想，不写代码、不改 `cc.ce`。
7. **不伪造溯源**：在签名/索引机制未扩前，不声称包经过源码级审计或供应链
   证明；CPKG02 签名只保证打包后负载未被篡改。

## 7. 风险与缓解

| 风险 | 说明 | 缓解 |
|------|------|------|
| ABI 表面过小 | 上游程序常用的 stdio/文件/时间/网络语义在 `catos_api_t` 中无对应槽 | 阶段 A 先做槽位覆盖核对；缺口集中记录，必要时按 append-only 流程评审扩槽；不达标源码不进闭环 |
| 镜像/栈预算超支 | 上游工具体积普遍超过 256 KiB，或使用大栈帧/大数组 | 阶段 B 脚本内置预算校验；阶段 C 实测镜像/bss；超支即裁剪或换源；禁止靠开大内核栈解决 |
| 宿主 multilib 缺失 | 误引主机 libc 头会直接编译失败（`bits/libc-header-start.h`） | 移植桩评审清单把「不引主机 libc 头」列为硬检查；CI/脚本里固定 freestanding 参数族 |
| 子集编译器阻塞 guest 编译 | `cc.ce` 不支持 struct 等主流结构，多数上游源码无法现场编 | 路径 E 严格限定子集小程序；主推路径 A，不承诺 guest 内编译通用上游 |
| 供应链与许可证 | 上游源码出处/许可证未登记，签名不覆盖源码 | 索引预留溯源字段；未补齐前在包描述与文档中明确标注「预编译负载签名，未做源码溯源」 |
| 网络拉包限制 | slirp 单连接约 80 KiB 空闲断连；源码 tarball 通常大于现成 `.ce` | 阶段 D 预编译入仓，guest 不拉源码；阶段 E 之前先评估源码包体积与 Range 续传边界 |
| 移植桩漂移 | 多 port 各自复制桩函数，行为不一致 | 重复实现先收进 `extapp/` 共享层；共享层改动走既有 CE 回归（e3/vprobe）门禁 |

## 8. 开放问题（实施前需裁决）

1. 溯源字段格式：`PKGS.TXT` 行式索引如何向后兼容地增加上游 URL/版本/许可证/
   指纹字段；guest 解析代码是否需要同步改。
2. 首个样例 port 的选型：优先控制台小工具（便于探针判定）还是图形/网络类，
   取决于阶段 A 清单的实际覆盖度。
3. 移植桩共享层的归属与命名：新增共享桩文件是否进 `extapp/` 现有共享编译单元，
   还是按 port 目录隔离。
4. 阶段 E 是否单独立项：guest 内编译依赖 `cc.ce` 能力扩充，工作量与本闭环
   解耦，建议不在 F5 最小闭环内排期。

## 9. 与既有设计/代码的对应关系

- `docs/pkg-repo-design.md` §2：仓库布局、交叉工具链表、六步流水线的早期
  预留；本文 §4 是其在 v2.82 约束下的分阶段落地分解，二者冲突时以代码现状
  （§1）为准。
- `docs/cc-design.md`：`cc.ce` 的语言子集、镜像布局与宿主仿真流程；本文
  §1.3 G3、阶段 E 直接引用其边界。
- `kernel_v25/extapp/build_ce.sh`：现有编译/链接/打包参数族，路径 A 阶段 B
  复用其 flags 与桩链接方式，不另造工具链。
- `tools/mkpkg.py` 与 `shell.c:pkg_install_file/pkg_verify`：打包与验签
  链路，阶段 D 原样复用。
