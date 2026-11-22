# CatOS CDE 模块化设计：可拆卸、可安装的图形桌面扩展

> 目标：把"图形桌面环境"从内核拆出来，做成一个像 Linux 桌面那样
> **可 pkg 安装 / 可卸载**的扩展。内核只提供通用图形原语，桌面语义
> （窗口、控件、任务栏、开始菜单、事件队列）全部随 `desk.ce` 一起装卸。
>
> 参考：另一位开发者的 CatOS CDE（`/d/xwechat_files/.../2026-11/isofiles.zip`），
> 已归档在 `deliverables/cde-ref/` 与 `github.com/ocean-2026/catos-cde`。
> 本设计**学其设计理念，不照抄其内核耦合架构**。

---

## 1. 为什么不能照抄参考实现

参考实现的 CDE 是**内核全功能 + 用户态外壳**的紧耦合架构：

- `src/cde.c`（2486 行 / 89 KB）把窗口管理、控件、事件泵、任务栏、
  开始菜单、壁纸、焦点路由全编译进内核；
- `catos_api_t` 里约 60 个槽是 GUI/CDE 语义（`cde_win_*`、`cde_poll_event`、
  18 种 `CDE_EVT_*` 等）；
- `desktop.ce`（1942 行）只是"声明 UI + 收事件"的薄外壳。

问题：桌面语义焊死在内核里，既不能装卸，也让 ABI  forever 膨胀。
这与 CatOS 的战略（图形模式 = shell 的图形后端，不是独立 GUI 体系）
和 CE ABI 的 append-only 冻结约束都冲突。

## 2. 我们的架构：内核只留通用原语，桌面整体上移

```
┌──────────────────────────────────────────────┐
│  可安装扩展包 desk.catpkg (CPKG02 签名)        │
│   └─ payload = desk.ce                        │
│       ├─ 窗口管理 / 控件 (retained 自绘)        │
│       ├─ 事件循环 (pollkey + mouse ABI)        │
│       ├─ 任务栏 / 开始菜单 / 桌面图标           │
│       └─ .cdeconf 配置持久化                   │
├──────────────────────────────────────────────┤
│  内核 CE ABI (append-only, 只加通用原语)        │
│   已有: gfx_rect/gfx_blit/gfx_info (61-63)     │
│   已有: pollkey(64) + mouse_* 全族 + mmap       │
│   新增: video_* / buf_* / key_peek 等 (§3)     │
├──────────────────────────────────────────────┤
│  渲染后端: be_text / be_gfx / be_vbe24         │
└──────────────────────────────────────────────┘
```

**内核退化成"帧缓冲 + 输入 + 计时"提供者**，和 X11 server 的角色类似；
桌面环境是一个普通 CE 程序，走 `pkg install` 装卸，与内核版本解耦。

## 3. 需要新增的 ABI 槽（append-only，追加到 65 号之后）

现状 66 槽（0..65，末槽 `gotoxy`）。参考调研结论：要让纯用户态 CE
完整实现桌面，最小新增 12 个通用原语槽。**注意先修复已有漂移**
（`ce.h` 内核镜像缺 65 号 `gotoxy`，`task.c:nargs[]` 也漏了槽 65）。

| 新槽 | 签名 | 用途 |
|---|---|---|
| 66 | `int key_peek(void)` | 窥视下一个 scancode 不消费（事件泵需要） |
| 67 | `int key_mods(void)` | Alt/Ctrl/Shift 修饰键位掩码 |
| 68 | `int video_info(int *w,int *h,int *pitch,int *bpp)` | 查询帧缓冲几何 |
| 69 | `int video_mode_set(int mode)` | 进入图形模式（幂等） |
| 70 | `int video_mode_text(void)` | 恢复文本模式 + 字库快照 |
| 71 | `int buf_create(int w,int h)` | 离屏缓冲（窗口客户区/双缓冲） |
| 72 | `int buf_destroy(int id)` | 释放离屏缓冲 |
| 73 | `int buf_blit(int id,int x,int y)` | 合成到屏幕（裁剪） |
| 74 | `int buf_rect(int id,...)` / `buf_text(...)` | 在离屏缓冲绘制（可选，也可复用 gfx + blit） |
| 75 | `int gfx_dirty(int x,int y,int w,int h)` | 显式脏矩形（配合 vbe24 脏检查） |
| 76 | `const char *clock_text(void)` | 内核时钟字符串（可选，也可用 `time_str`） |

> 落地时按"最小可用"分期：P1 只需要 `key_peek/video_info/buf_create/
> buf_destroy/buf_blit`（5 个）就能跑通一个窗口 + 事件泵；其余按需追加。
> 每加一批都要三处同序同步（见 §5）。

## 4. 可安装性设计

### 4.1 扩展包格式

复用 CPKG02（v2.68 已实现 RSA-512 签名），payload 是 `desk.ce`：

```
/pkg/desktop.catpkg  → pkg install desktop → /bin/desktop.ce
```

`build/pkg.list` 增加一行：`desktop|1.0|detachable desktop environment|desktop.catpkg|desktop`。
扩展本身**不进 `build/apps.list`**（不随内核内置），只有 `pkg install` 后才出现在 `/bin`。

### 4.2 命令与启动

- `pkg install desktop`：安装桌面扩展（验签 → 写 `/bin/desktop.ce`）；
- `desktop` 命令：shell 分发到 `ce_run("/bin/desktop.ce")`，进入图形桌面；
- 桌面内 `Start > Shut Down` 或 Esc → `cde_exit` → 回文本 shell；
- `pkg remove desktop`：删除 `/bin/desktop.ce`，系统回到纯命令行，**内核不变**。

### 4.3 生命周期状态

安装状态靠 `/bin/desktop.ce` 是否存在判定（与现有 `pkg_installed()` 一致），
无需新增注册表项。偏好配置 `.cdeconf` 由 `desk.ce` 自己读写（复用 `save/cat` ABI）。

## 5. ABI 追加的三处同步（血的教训）

参考 `CDE_NOTES.md` 记录过"三处顺序不一致导致所有后续函数指针错位"。
CatOS 同理，追加每个槽必须同时改：

1. `include/ce_abi.h` — 用户侧 `catos_api_t` 结构末尾追加；
2. `include/ce.h` — 内核侧镜像结构末尾追加（当前缺 65 号，先补齐）；
3. `src/task.c` — `enum SYS_*`、`nargs[]`、`isr_syscall` 的 `case`、
   `build_vsyscall_page` 的 thunk 数（2 KiB 区上限约 85 槽）；
4. extapp 侧 `ce_app.h`/`ce_app.c` 自动跟随，无需改；宿主桩 `host_stub_go.c`
   若做完整结构初始化需补成员。

## 6. 从参考实现吸收的关键经验（不踩重复的坑）

| 坑 | 我们的规避 |
|---|---|
| `cde_poll_event` 每轮整屏重绘 → 撕裂 | 桌面用脏矩形 + 只在事件变化时合成 |
| 字体用 Consolas 8x8 阈值化糊成乱码 | 复用 CatOS 自带真 IBM VGA 8x16 ROM（`font_ascii.inc`） |
| 编辑器打字整屏闪 | 影子缓冲 + 屏幕差分，只写变化的 cell |
| 后台窗口输入框抢按键 | 焦点窗口归属检查（`cde_focused_win` → 我们的焦点状态） |
| `^A/^U/↑/↓` 被输入框吞 | 这些键不消费，推给应用做行编辑 |
| 函数参数 >6 个变栈垃圾 | 新 ABI 参数控制在 6 个以内（已有 `gfx_blit` 用结构体指针） |
| 编译列表两处漏加 `.o` | 我们的 `build.sh` 三处硬编码列表要同步（若加内核侧文件） |

## 7. 分期路线（每期都过 QEMU 实机回归）

- **P1 原语补齐**：修 ABI 漂移（65 号）+ 追加 5 个最小 GUI 原语槽；
  写一个 `win_demo.ce` 验证开窗/绘制/事件（先在宿主原型）。
- **P2 桌面外壳**：移植参考 `desktop.c` 的事件循环与窗口模型到纯 CE
  （retained 控件自绘），做 `desk.ce`；`pkg install` 装卸验证。
- **P3 完整外壳**：任务栏/开始菜单/桌面图标/文件管理器/终端/编辑器，
  吸收参考 v0.11–0.13 的全部经验。
- **P4 可拆卸性回归**：装 → 用 → 卸 → 系统回到纯 shell，内核二进制
  前后一致（证明桌面语义确实不在内核里）。

## 8. 当前状态（v2.69 / P1 已落地）

- 参考源码已解包至 `deliverables/cde-ref/upstream-isofiles/`；
  分析结论见本设计 §1/§6。
- 架构路径已定：纯 CE 程序 + pkg 安装（用户 2026-11-21 确认）。
- 已按"新工具先出原型"规则写宿主命令行原型
  `deliverables/cde-prototype/cde_shell.py`（生命周期）与
  `gfx_primitive_proto.py`（窗口/绘制/事件循环），两者 `--demo`/直接运行
  均可看到完整效果。

### P1 实际实现（与 §3 原计划的差异）

经盘点，参考实现桌面依赖的核心底层能力是**鼠标状态 + 文本绘制 +
键盘事件**。现有 CE ABI 已有 gfx_rect/gfx_blit/gotoxy/pollkey，唯一
完全缺失的是鼠标。因此 P1 按"最小可用"收窄为：

1. **修 ABI 漂移**：`include/ce.h` 内核镜像补槽 65 `gotoxy`；
   `task.c:nargs[]` 补槽 65 的 `nargs=2`（此前隐式 0 → thunk 不转发
   ebx/ecx，`gotoxy(x,y)` 拿到陈旧参数）。实机 `tests/gotoxy_v269_probe.py`
   验证标记精确落在 (10,3)，2/2 PASS。
2. **追加 3 个通用鼠标原语槽**（append-only，66/67/68）：
   `mouse_x()/mouse_y()/mouse_buttons()`，均薄包装内核 `mouse_get_state()`。
   三处同序同步：`include/ce_abi.h`、`include/ce.h`、`task.c` 的 enum/
   nargs/isr case。`tests/mouse_abi_v269_probe.py` 验证 CE 读数与内核
   `mouse` 命令状态完全一致，4/4 PASS（本 QEMU 无 PS/2 鼠标，事件注入
   检查按环境跳过）。
3. **`desk.ce`（源 `extapp/desktop.c`）**：纯用户态桌面，全部窗口语义
   （焦点/命中测试/任务栏/事件循环）在 CE 侧，内核只提供原语。当前用
   text 后端 ASCII 窗口（`gotoxy`+`write`），可在 QEMU/VMware 目视与
   text buffer 探针验证。
4. **可安装扩展包**：`build/pkg.list` 增加 `desktop|1.0|...|desktop.catpkg|
   desktop`，复用 CPKG02 + RSA-512 签名；`pkg install desktop` 验签后写
   `/bin/desktop.ce`，裸名 `desktop`（PATH=/bin）直接运行，无需改 shell。
   `tests/desktop_pkg_v269_probe.py` 完整生命周期 **10/10 PASS**：
   list→install→运行桌面→q 退出→remove→命令消失，内核二进制不变。

### 尚未实现（P1b / P2）

- §3 原计划里的 `gfx_text`、`key_peek`、`key_mods`、`video_*`、`buf_*`
  尚未追加。当前桌面在 text 后端；gfx 24bpp 文字绘制需要 `gfx_text`
  （用 `font_ascii` 8x16 位图逐像素写 `gfx24_pixel`），列为 P1b。
- P2：把窗口做成 retained 自绘 + 脏矩形合成，移植参考实现的开始菜单/
  文件管理器/编辑器，切到 gfx 后端。
- 环境注：本 QEMU 无 PS/2 鼠标（`mouse` 命令 pk 恒 0），鼠标事件注入类
  验证需在带鼠标的 VMware/真机完成；ABI 接线已由 CE==内核状态断言证明。

---

## 9. v2.73 地基修复：ring0 sshd 阻塞 CE 键盘事件（2026-11-22）

### 9.1 症状与定位

v2.72 上 `pkg install desktop` → `desktop` 能装能渲染，但 `q`/`Esc`/`Ctrl+C`
全部无效，**进桌面后只能重启**。v2.69 探针声称的 "10/10 PASS" 是**假阳性**：
它用 `"root@catos" in screen_text()` 判断"已退出"，而桌面用 `gotoxy` 画在
text buffer 上，旧提示符残留直接命中断言。

定位手段（可复用）：写最小 CE 探针 `extapp/keyprobe.c`（阻塞 `getch()` +
非阻塞 `pollkey()` 打印返回值），用 `inject_any.py` 注入测试盘，跨版本二分。
结论：**v2.68.3 及以前正常，v2.72 收不到任何键**——是内核回归，不是
desktop.c 应用 bug，也不是 QEMU 环境。

> 二分过程踩坑：批量连跑多个 QEMU 实例时残留进程导致端口/焦点干扰，
> 出现"v2.69/v2.71 也坏"的假象。每次测试前必须 `taskkill qemu-system-i386`
> 并用独立 monitor 端口，否则结论不可信。

### 9.2 根因

v2.72 新增 `/etc/autoexec/sshd.sh` 自启动 ring0 daemon（`task_spawn_r0`）。
该 daemon 常态停在 `!net_up()` 分支，用 `task_maybe_yield()`（cooperative
busy-yield）让出 CPU。当 ring3 CE 任务阻塞在 `SYS_GETCH` 内时，`g_in_syscall`
全局计数器停在 1，令 `task_maybe_yield()` 直接 return——sshd 于是不再让出，
独占 CPU，键盘 IRQ 唤醒的等待任务永远得不到调度，CE 的 `pollkey`/`getch`
收不到键。ring0 shell 走 timer 睡眠路径不受影响，所以命令仍能输入。

### 9.3 修复

`kernel_v25/src/ssh.c` 的 `sshd_daemon_main()`：无网卡等待分支改用
`task_sleep_ticks(1)`（进 `T_SLEEPING`，由 timer IRQ 唤醒 + idle park 路径），
不再依赖 cooperative yield。改动一行，保留 sshd 自启动功能。

> 尝试过但**无效**的方案：改 `task_maybe_yield()` 让 ring0 忽略
> `g_in_syscall`——单独改这处键盘仍不通，因为 cooperative 切换本身
> 无法恢复等待任务。已回退，不要重试。

### 9.4 验证

- `keyprobe.c`：`getch=979`('a')、`pollkey=111311`('q')、正常 `exit(0)`。
- CDE 桌面：`desktop` → `q` 退出回 shell → `pkg remove` → 命令消失，全通过。
- `ssh_v272_probe.py`：4/5 PASS；唯一 FAIL（host key 重连严格检查）在原版
  v2.72 ISO 上同样 FAIL，属宿主 ssh 客户端环境问题，与修复无关。

### 9.5 遗留隐患（本次未改）

`net_tcp_saccept()`（netstack.c）内有 3000000 次 `task_maybe_yield()` 忙循环，
有网卡时 daemon 会走这里，理论上有同类键盘饥饿风险。该方法被前台交互
`sshd` 共用，直接改会破坏其 ESC/超时语义，故本次不动。带网卡的 VMware
上若复现"进桌面后键盘失灵"，优先查此处。

---

## 10. v2.73 gfx24 真图形后端调查（进行中）

### 10.1 目标与现状

P1b 目标：把 CDE 从 text 后端 ASCII 窗口升级为 gfx24 真彩图形桌面。
现有 `desktop.c` 仍是 text 后端（`gotoxy`+`write`），P1b 的 `gfx_text`/
`buf_*` ABI 槽未追加。**更正上轮结论**：远端那批 v2.72 提交里的
"canvas colour"只是主题配色单一数据源（`colors` 命令 + hive），
与"画布应用"无关，P1b 并未被别人完成。

### 10.2 关键发现：CE 侧渲染完全可行（零 ABI 改动）

内核 gfx24 后端已有 `render_font_glyph` 字体渲染循环，只是没暴露给 CE。
验证了一条**零内核改动**的路子：CE 把真 IBM VGA 8x16 字体
（`font_ascii.inc`）编进镜像，在离屏 RGB24 buffer 上逐像素画字，
用**现有** `gfx_blit`（槽 63）一次投屏。

- 原型：`kernel_v25/extapp/gfxtext.c` + `tests/gfxtext_probe.py`。
- 结果：`gfxtext.ce` 实机画出 5 行彩色文字，字形锐利清晰、无残缺错位
  （`tests/dumps/gfxtext_zoom.png` 目视确认）。**证明 P1b 渲染不需要新 ABI**。
- 完整桌面原型：`kernel_v25/extapp/gfxdesk.c` + `tests/gfxdesk_probe.py`，
  画 800x600 背景、任务栏、三个窗口、图标，`gfxdesk_probe.py` 像素断言 4/4 PASS。

**两个构建坑（务必记住）**：
1. CE 的 bss 上限 256 KiB（`ce.h:CE_MAX_BSS`）。离屏 buffer 必须 ≤ 216 KiB；
   384x192x3=216 KiB 可用，448x224x3=301 KiB 会被 mkce.py 拒绝。
2. **CE 源文件里绝不能重定义 `__ce_api`**。`ce_app.c` 已提供 weak 定义；
   再加一个强定义会让 `build_ce.sh` 的 nm 正则选中错的 `.ce_api` 单元格，
   内核写入另一个槽 → 程序读到 NULL → `pollkey` 处 page fault
   （`cr2=0xf4` = API 槽 61×4）。`gfx24.c` 是老代码仍带强定义，别照抄。

### 10.3 阻塞 bug：gfx 模式下 ring3 的 task_sleep_ticks() 不返回

排除了原型逻辑问题后，做了一个干净对照：把已证明 text 模式下键盘
3/3 PASS 的 `keyprobe.c` 改成先 `display_mode(3)` 切 gfx24——同样的代码
立刻 0/3（getch/pollkey 全收不到键）。**这是内核级 bug，不是桌面应用问题。**

二分定位链路：
- 切 gfx 后 CE 能画画面（gfx_rect/blit 成功）→ 切换本身和 API 表正常。
- 但 `q` 之后 CE 无任何响应；把循环心跳绑到任务栏颜色后确认：
  任务栏停在初始 compose 的蓝色，第一次循环迭代的 `api->delay(30)`
  之后的绿色重绘**从不执行**。
- `api->delay` → `task_sleep_ticks()` → `task_block(T_SLEEPING)`，靠
  IRQ0 的 `sched_timer_tick()` → `task_tick_wake()` 唤醒。
- 定位到 `kernel_v25/src/task.c:sched_timer_tick()` 的 ring3 抢占分支：
  中断 ring3 任务时走主动 `pick` 切换；空闲时（`c->ring0`）只置
  `need_resched`。怀疑 idle park 路径在 gfx 模式下没把唤醒的 sleeper 切回 CPU。

> 尝试过但**无效**：忙轮询版（去掉 delay）反而画不出画面，怀疑高频
> syscall 触发了另一处问题。不要在这条路上继续。

### 10.4 交付状态与下一步

- 已交付：`gfxtext.c`/`gfxdesk.c` 原型 + 两个正式探针；渲染能力已证实。
- XFAIL（非 FAIL）：`gfxdesk_probe.py` 标注键盘退出因上述内核 bug 暂不可用。
- **下一步（需专项调查）**：在 `sched_timer_tick()` ring3 唤醒路径加诊断，
  确认 gfx 模式下 `task_tick_wake()` 是否把 sleeper 置 READY、idle 是否
  真正调度它。这是浏览器/多媒体查看器等交互应用的前提，建议单独立项。
