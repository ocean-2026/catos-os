# CatOS 跨夜交付报告（2026-11-24 / 25）

> 状态：**三门禁在当前 HEAD 复跑全绿，v2.80 双 ISO 已重出并重记 sha256**。
> 本报告只写本轮在合并树实测到的结果；未在当前树复跑的项一律标 `PENDING`，
> 不写「已并入 / 已验收」。desk 线（`BLK_BUTTON/BLK_INPUT/BLK_TERM`、Settings
> `gui style` corner、`gui term`）**尚未合入**本分支。
>
> 分支：`feat/gui-blocks`；HEAD `605ea25`。TLS 野站与浏览器 v0.1 的真站结果以
> 用户线 / 独立工树实测为基线，本合并树复跑结果见 §4 / §5 的 `PENDING` 标注。

## 1. 合并提交链（feat/gui-blocks）

| 提交 | 类型 | 说明 | 双亲 |
|---|---|---|---|
| `6df891d` | merge | gui-v2 并入（`BLK_FREE`/溢出三态/拖拽缩放/cv_dirty 页池 64） | `76d0f64` + `4184b1c` |
| `d797fee` | merge | feat/browser 初版并入（重定向 6 跳/内联 CSS 盒模型/`br` active-result） | `6df891d` + `b70979b` |
| `c15bccf` | merge | TLS 野站真通（ChaCha20-Poly1305 + ECDHE-ECDSA P-256 + sigalgs 全集） | `d797fee` + `a403984` |
| `9cf8004` | merge | 浏览器 v0.1（chunked/动态扩容 288 KiB/BMP 同源图/CSS 百分比/max-width） | `c15bccf` + `3b8dcf3` |
| `9156818` | docs | 交接快照（01:25 巡检与 TLS/浏览器合并记录） | `9cf8004` |
| `7aeaa2c` | merge | 桌面线合并基线 | `9156818` + 桌面线 |
| `95395b8` | test(gui) | GUI 交互上溯 | `7aeaa2c` |
| `44226d3` | test(gui) | menuclick 探针补 live_before/after 块计数遥测 | `95395b8` |
| `22ad175` | test(gui) | mouse_smoke 补 BLOCK-1/2 硬断言 | `44226d3` |
| `33d074e` | release | 版本串切 v2.80（唯一用户可见版本串 `shell.c:1049`） | `22ad175` |
| `8fea31e` | fix(link/paging) | GDT/TSS/IDT 固定槽位 + 任务帧恢复（修 ring3 三重故障） | `33d074e` |
| `08db94e` | test(trace) | spawn/sched 面包屑（`CATOS_SPAWN_TRACE` 默认 0，不进 release 行为） | `8fea31e` |
| `605ea25` | fix(link) | `.bss` 跨 arena 由 fatal 降为 warn，仅 KEY_TABLES 保持致命 | `08db94e` |

> desk 线 `feat/gui-desk` `c2ad71a` **未合并**；`feat/tls-chacha-wip=732abc6` 为用户线备份，
> 未单独合并、未 reset。stb PNG/JPEG 接入仍阻塞于 32 位软浮点运行时，未入构建。
> 本地三个修复/测试提交（`8fea31e`/`08db94e`/`605ea25`）**尚未 push**。

## 2. 三门禁结果（当前 HEAD `605ea25`，test 镜像实测）

测试镜像：`kernel_v25/catos-v2.80-test.iso`（`GUI_AUTOSTART=0`，停 prompt），
sha256 `8533f41e4635c367cfee9efa89ea8550377d6c96001c15dcb51b0668df731e88`。
2 核 TCG，严格串行一次一个 qemu。

| 门禁 | 结果 | 耗时 | stdout 证据（artifacts/gui-blocks/） |
|---|---|---|---|
| vprobe | **10/10**（format + bin-has-vprobe + 8 case） | 154.5s | `gate-vprobe-full-0647.log`；串口 `gate-vprobe-full-0647-serial.log` |
| E3 | **2/2**（badptr / conc） | 80.2s | `gate-e3-full-0647.log` |
| F11 | **9/9**（passwd/登录/权限/掩码/盐值差异等 9 项） | 161.8s | `gate-f11-full-0647.log`；文本转储 `logs/f11/` |
| `check_link_layout.py --strict` | **RC=0**（KEY_TABLES 全在线下；普通静态跨 arena 为 warn） | — | `kernel_v25/tools/check_link_layout.py build/kernel.pe --strict` |

- vprobe 逐 case 证据链：`gate-vprobe-postfix-0626.log`（fr/cat/hw/str/strcpy/wr/fw/pw 全 PASS，含 elapsed 与串口路径）。
- 历史基线（旧提交树，供对照）：vprobe `6df891d` 10/10（155.0s）、`d797fee` 10/10（157.1s）、
  `c15bccf` 10/10（156.9s）；E3 2/2 约 80.1–80.3s；F11 9/9 约 161.8–161.9s。
- lint：本轮未重跑静态分析器；历史基线为 0 violation（advisory/review 不计致命），沿用旧记录，不据此判本轮。

## 3. 串口探针（合并树实测 PASS）

| 探针 | 判定 | 关键打点 |
|---|---|---|
| `gui free` | **PASS**（anomalies 0） | `result=1`；A–E 五块锚点/百分比/min-max 解算矩形 |
| `gui cvdirty` | **PASS**（anomalies 0） | `outside_bad=0 inside_miss=0 result=1`，帧 379 µs |
| `gui interact` | **PASS**（anomalies 0） | `grew_l/r/t/b=1 min_ok=1 max_ok=1 clamped=1 persist=1 result=1` |
| `gui sak` | **PASS**（anomalies 0） | `panel=1 events_blocked=1 hit_panel=1 leaked=0 result=1` |
| `gui decl` | **PASS**（anomalies 0） | `result=1 leaked=0`，拒绝码 `-36`（越层） |
| `gui cvchan` | **PASS**（anomalies 0） | `result=1`，未知句柄 `-34` |
| `gui cv` | **PASS**（anomalies 0） | `active=1 result=1`，帧 1375 µs |
| `gui br` | **PASS** | `result=1 active=1` |
| dektest（ring3） | 基线 **PASS** | `commit=0 poll=0 tw=16 img=-38 result=1`（74 无样图返 -38 属预期） |

### 像素组
| 组别 | v2 独立工树基线 | 合并树复跑 |
|---|---|---|
| free 像素组 | 3/3 PASS（v2 归档） | `PENDING` |
| interact 像素组 | 3/3 PASS（v2 归档） | `PENDING` |
| 桌面默认组（M0 三条） | 历史基线 | `PENDING` |
| browser 截图组 | 见 §5（独立工树 acceptance） | `PENDING` |

> 像素基线色值 / idem 哈希 / damage 占比：待最终树 `gui_pixel_regress.py` 与 v2 样例帧复跑后回填。
> v2 基线参考：idem 哈希 `0x88751F07 match=1`；drag 帧 1.54 ms ≤ 4 ms；damage ≈ 2.0% ≤ 25%。

## 4. TLS 野站

### 4.1 能力（合并入 `c15bccf`）
- 套件：`0xCCA8` ECDHE-RSA-Chacha20-Poly1305-SHA256、`0xCCA9` ECDHE-ECDSA-Chacha20-Poly1305-SHA256；
  ECDHE secp256r1（vendored micro-ecc），ECDSA P-256 叶公点 + SHA-256 签名校验；sig_algs 20 项；ChaCha20 写 IV。
- 串口打点：握手记录 `TLS:rec msgs=<n> port=<p>`；curl HTTPS 成功 `TLS:wild status=<st> bytes=<n>`。

### 4.2 真站结果
| 站点 | 用户线实测基线（独立工树） | 合并树 `tests/tls_wild.py` 复跑 |
|---|---|---|
| `https://example.com` | **200 / 577 B** | `PENDING` |
| `https://www.baidu.com` | **200 / 227 B** | `PENDING` |
| 本地强制握手 `0xCCA8` | **PASS**（用户线） | `PENDING` |

- 回归探针：`tests/tls_wild.py`（必需门 example.com 200 + 非空 body；baidu 次级记录）；
  产物 `artifacts/d2-tls/wild_result.json`、`wild_serial.log`、`wild_<host>.ppm`。
- 本地强制套件探针 `tests/tls_probe.py`（`0xC027`）语义未改。

### 4.3 遗留说明
- **CBC `0xC027` 遗留缺项**：Finished 首条加密记录 `decode_error`，由用户继续排查；
  **不影响 AEAD（ChaCha20-Poly1305）野站通路**。本分支未改 CBC 路径。

## 5. 浏览器 v0.1

### 5.1 能力（合并入 `d797fee` + `9cf8004`）
- 重定向最多 6 跳；响应体走流式接收器，HTML 缓冲按需帧分配扩容至 288 KiB；
  chunked 解码；BMP/PPM 同源图片解码；内联 CSS 盒模型最小集（padding/margin/border/对齐/颜色/尺寸）+ 百分比/max-width。
- 打点：`GUI:br bytes/nodes/blocks/ms/status/active/result`。

### 5.2 站点 acceptance（独立工树 `browser-accept.json` 基线）
| 站点 | 类别 | 判定 | 打点 |
|---|---|---|---|
| `http://example.com/` | net HTTP | **PASS** | `bytes=577 nodes=10 blocks=4 ms=1330 status=200 active=1 result=1` |
| `sample://local` | offline | **PASS** | `bytes=300 nodes=12 blocks=12 status=0 active=1 result=1` |
| domestic | net HTTP | SKIP | 未配置 `CATOS_BR_DOMESTIC` |
| `https://example.com/` | https | SKIP（浏览器线当时） | HTTPS 通路待 TLS 修复；现 TLS 已合并，复跑 `PENDING` |

- 截图（独立工树 overnight `shots/`）：`http-example.png`、`offline-sample.png`、
  `https-example-blocked.png`、`browser-bmp.png`。

## 6. 本轮修复：ring3 `.ce` 启动空屏根因

合并基线 `7aeaa2c` 下，环3 `.ce` 进入后无 syscall、无 fault、全屏空白。前期已排除 `.bss`
体积单一因素（term.c 帧分配实验已还原 HEAD）。经 `nm` 对照与 qemu `-d int` trace 二分，
定位到两处同类静态布局问题，均已在 `8fea31e` 修复。

### 6.1 GDT 落进用户映像区（三重故障）
- 合并把 `.bss` 顶高，`_gdt` 落到 `0x02000000`（恰好 `USER_IMAGE_BASE` / CE 装载基址）。
  任务 CR3 下该 VA 被重映射到用户帧，环3 经 GDTR 取 UDATA 描述符读到映像页 →
  `#GP(e=0020)` → `#DF` → CPU Reset。父分支 a851da7 的 `_gdt=0x01ff2ee0` 在安全线以下故正常。
- 修法：`pe.ld` 在 `.bss` 起始显式预留 `_gdt`(48B)/`_idt`(2048B)/`_tss`(104B) 固定槽位；
  `intr.c` 的 gdt/idt 改 `extern`（不定义存储）；`cpu.s` 移除 `_tss .skip 104` 定义。
- 修后符号地址：`_gdt=0x0114b000`、`_idt=0x0114b030`、`_tss=0x0114b830`，均 `<0x2000000`。

### 6.2 镜像 PTE 缺 US 位判断（环3 保护错）
- GDT 修复后 CE 能进环3 并打出 3 次 syscall（`STRCMP(20)→ALLOC(11)→FILE_READ(39)`），但卡在
  FILE_READ。int trace 末向量 `v=0e #PF`，错误码 `e=0005/0007`——解码为 **P=1（present）、U=1**，
  即环3 访问 supervisor-only 镜像页的**保护错**，非缺页。根因：合并后 `frames_base` 抬高到
  `0x2104000`，堆/栈 VA `0x2100000..0x2103fff` 落在内核静态帧上，#D11 镜像为这些帧装了
  supervisor-only PTE（P=1, US=0）；而 `heap_ensure_range`/brk/`vma_map_page_on_fault`/
  `validate_uaddr` 原先只按 present 判断「已映射」，把镜像当成用户页跳过装帧，槽位保持 supervisor-only。
- 修法：`paging.c` 新增 `task_page_is_user(s, vaddr)`（要求 PTE `present + US(0x004)` 才视为用户已映射）；
  上述四处「已映射」判定由 `task_virt_to_phys` 改为 `task_page_is_user`，镜像槽位会被重映射到新鲜用户帧。

### 6.3 bss 搬迁决策（已否决大规模搬迁）
- 曾评估把 ≥4KiB 缓冲惰性取帧以压 `_bss_end < 0x2000000`。清单显示危险带占用 1,061,028 B，
  可搬迁 ≥4K 项 864,552 B，跨 hive/term/tls/bench 十几个文件；磁盘路径核心表
  （`fat_cache`/`fat_journal`/`e3_fr_buf`/`fbuf.4`）已在 0x2000000 以下。
- 结论：**不搬 bss**，根因是镜像 US 位判断而非 bss 体积。护栏把 `_bss_end` 越界从 fatal 降为 warn；
  仅 KEY_TABLES（gdt/idt/tss/acl_tab/users/tasks/pipes/cur_task）保持 `--strict` 致命（`605ea25`）。

### 6.4 API.TXT 定性（既存，非合并引入）
- `/appdevelop/API.TXT install FAILED (disk full?)`。静态比对：该文件在 a851da7 已存在、同 34894 字节；
  `fat.c`/`fs.c`/`builtin_apps.c` 与父分支逐字节一致（无合并 diff）→ 判**既存**，非合并引入。
- 旧 ISO 实证 A/B 被阻塞（legacy gui-iso 在当前驱动下串口空白），结论基于源码 + 尺寸 diff。

## 7. v2.80 双 ISO（已重出并重记 sha256）

门禁全绿后，按两种配置分别重建。ISO 只放 `kernel_v25/` 与 `artifacts/overnight-20261124/`，
`.iso`/`.img` 被 gitignore，不进提交。

| 变体 | 编译开关 | 用途 | 文件名 | sha256 | 字节数 |
|---|---|---|---|---|---|
| 调试/门禁版 | `GUI_AUTOSTART=0`（默认），开机停 prompt | vprobe/E3/F11 与探针目标 | `catos-v2.80-test.iso` | `8533f41e4635c367cfee9efa89ea8550377d6c96001c15dcb51b0668df731e88` | 12,079,104 |
| 桌面自启动版 | `GUI_AUTOSTART=1`，盘内无 `/etc/autostart` 时内置默认进块桌面 | 交付盘（配对种子盘） | `catos-v2.80.iso` | `c6c2d19f0f5258695668956cf2be1b748488615847b97b9efba991ecc126db1f` | 12,079,104 |

- 构建：`cd kernel_v25 && sh build.sh iso`；自启动变体 `GUI_AUTOSTART=1 sh build.sh iso`。
- 自启动优先级（`shell.c:7784` 起）：盘内 `/etc/autostart` 存在（非空）→ 执行它；否则编译开关
  `GUI_AUTOSTART=1` → 进块桌面；否则停 shell。门禁/探针镜像必须停 prompt，故固定 `GUI_AUTOSTART=0`。
- release 变体未跑三门禁（自启动会直接进桌面，会冻掉探针）；与 test 变体仅 shell 自启动分支不同，
  paging/GDT 修复与门禁版同盘同源。
- **旧坏盘处置**：06:11 修因前的两张带 ring3 三重故障的老构建已改名加 `-stale` 后缀，原名不再保留：
  - `catos-v2.80-stale.iso`（sha `087caa0ff61d1d11cb12675b0c0e3d8e33c76cbf31e078e54b9de3213936093a`）
  - `catos-v2.80-test-stale.iso`（sha `c3447bfb34977b59f902b31a70906a81d5d02b7386b230a194371e3756f957ee`）
  - artifacts 归档目录下同名旧盘已被新 ISO 覆盖，无残留同名旧盘。
- 交付盘现场 `--seed-autostart` 生成；`kernel_v25/catos_disk.img` 已 checkout 回 HEAD，sha256
  `8ac9d54b6b2f482a7b8753177d8714567c0af2a2e4208bb6d03e4f68abfa08a1`（67,108,864 B），未改动。

### 7.3 main 合并一致性（ff-only）
- main 已合并：main=`605ea258041abd7c1b1800a858062f7633613b9d`，tree=`48128f42f7454f1ad27a80f94577bbe4fe89a7d7`，
  双 ISO（`catos-v2.80.iso` `c6c2d19f…26db1f` / `catos-v2.80-test.iso` `8533f41e…31e88`）与 main 一致，无需重打，sha 不变。
- 合并方式：`git merge --ff-only feat/gui-blocks`（无 merge commit）；两侧 commit 与 tree hash 逐一比对相同。

## 8. 未决 / 已知限制 / PENDING 清单

1. desk 线合并：`BLK_BUTTON/BLK_INPUT/BLK_TERM`、Settings `gui style` corner 圆角、`gui term` 走
   `shell_exec`；Settings `-6` 菜单动作与 v2 探针分发并集；`con_gui_owned` 闸门与 hotfix `gui_begin`
   收摊顺序保持不丢。
2. 全量 GUI 探针与像素回归（free/interact/desktop/browser 组）在最终树复跑；像素色值/idem 哈希/damage 占比待回填。
3. 真站回归：`tests/tls_wild.py` 两站 + `0xCCA8` 本地握手；浏览器 HTTPS 站复跑。
4. 真鼠标 `mouse_smoke`：BLOCK-1 穿模、BLOCK-2 点 `z>0` 行整块消失，合并态 ISO 未复验。
5. calm 档证据：`GUI:style mode=calm` 串口行 + 全块 G2 像素组 + `/etc/style` 重启读回。
6. stb PNG/JPEG：待 32 位软浮点助手或定点替代；74/75 当前仅 BMP/PPM。
7. **已知限制（写实）**：
   - PNG/JPEG 软浮点阻塞；
   - SMP AP / 多核调度 deferred；
   - AHCI IDENTIFY ABRT 未通；
   - calm 示例块几何左偏约 40 px；
   - CBC `0xC027` Finished 首条 decode_error（不影响 AEAD 野站通路）。
8. **未做**：`D:\CatOs` 本地同步：主机下线，未做。

> 注：§8 第 1–6 项的 PENDING（TLS 两站、浏览器 HTTPS、mouse_smoke 7/7 在 v2.80 镜像复验、全量像素组等）
> 保持 PENDING，本次合并 main 不视为已复验通过。

## 9. 环境与口径备注

- 每次测试前 `pkill -9 -x qemu-system-i386`（勿用 `pkill -f qemu`，会匹配自身 shell）。
- 链接护栏参数为 `build/kernel.pe`（ELF 中间产物），非 `kernel.bin`；`--strict` 仅 KEY_TABLES 致命。
- 回调只打脏位、不嵌套提交；生产帧节流 100 tick ≤ 33 帧、`double_commit=0`；`.bss` 大数组走帧分配/arena。
- CE ABI append-only：槽 0–71 不动，新槽 72 起（72 commit / 73 poll / 74 image_load / 75 image_free / 76 text_width）。
- 环境：2 核 TCG，严格串行一次一个 qemu；外部 RPC 偶发 `710020702 resource center sign result empty`，
  等 8–20s 单次重试，勿循环。qemu 静态需 `-L /home/user/qemu-local/root/usr/share/qemu`。
- 未跟踪禁提交/禁入构建：`kernel_v25/src/gui/stb_image.c`、`kernel_v25/third_party/`；
  `docs/跨夜任务-交接.md` 用户自补不提交；`artifacts/` 不进提交。
- 诊断 trace（`08db94e`）由 `CATOS_SPAWN_TRACE` 宏包裹，默认 0，不改变 release 行为。
