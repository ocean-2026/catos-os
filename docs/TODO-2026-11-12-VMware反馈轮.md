# CatOS 待办事项（2026-11-12，VMware 实测反馈轮）

来源：用户在 VMware Workstation 17.6.1 里实测 CatOS 的 9 点反馈 + 本次会话遗留。
现状基线：v2.18（LFN 已入库 `9ca51c1`），回归 user 15/15、disk 5/5、qemu 12/12、
security 17/17 全绿。

> **状态更新 2026-11-13**：本轮全部开发在**云主机（Linux, env `vm`）**完成。
> v2.18 源码经 CDN 从 Windows 侧传入，构建链改造后 kernel/iso/disk 三个产物均重建成功，
> QEMU 冒烟启动通过。逐项进度见文末 **五、2026-11-13 执行进度**。

---

## 〇、已完成（本轮）

| 项 | 结果 |
|---|---|
| lsuser 表格对齐 | CAPS 列 8 位补零（`9ca51c1`） |
| 非 8.3 文件名（LFN） | VFAT 长名读写/删除/列表全支持，CJK 文件名经 UTF-16 可用；修复目录扩展后 0x00 标记滞留链中导致后续条目"隐身"的 bug（内置应用 38 项全部装上，v2.18） |
| 开机自启动目录 | `/etc/autoexec/`（8.3 名限制当时存在；LFN 入库后目录名可自由，但 autoexec 名字保留以免破坏文档约定） |

## 一、本次 9 点反馈判定与拆解

### 1. 启动页面是否正确 —— ✅ 正确
autostart 脚本输出 banner（CatOS 0.3 / 架构 / 引导器 / 文件系统）+ 网卡探测失败提示
+ `root@catos:/$`，全部符合预期。唯一问题是网卡提示文案是 QEMU 专属
（"add to QEMU: -netdev ..."），在 VMware 下是误导 → 归入 #54 按虚拟机类型给提示。

### 2. 后台任务输出管理 —— 拆为任务 #59
现状：`cmd &` 后台可跑，但输出无管理。目标：
- 后台任务 stdout 进每任务环形缓冲（不阻塞命令输入、不在前台穿插刷屏）
- `jobs` 列后台任务（tid/命令/状态）
- `log TID` 一次性查看输出；可选 `log -f TID` 持续跟随

### 3. 图形模式文字渲染 —— 拆为任务 #58
截图判定：720x400 高分下 8x16 字号偏小、显瘦/扁（缺 VGA 9px 字距与列间隔）。
- 字号/比例对齐文本模式观感（9x16 字距或 2x 缩放策略）
- 删除线位置下移（现压在字符中腰）
- 行间距加大
- 换行后旧行末残留光标下划线 → 清除
- 软件光标闪烁（timer 翻转）
- 等宽确认：CJK = 2×ASCII（现渲染已按字形宽表驱动，需补逐像素断言进回归）

### 4. 返回文本模式后颜色异常 —— 拆为任务 #57（P1，观感强相关）
`text` 退出图形模式：要求自动 clear；当前返回后文字全灰/白——图形模式动过
DAC 调色板/AC 控制器后未恢复 VGA 文本调色板。修法与 #100（vdiag 调色板）同类：
退出序列里重载标准 16 色 DAC + AC 重触发 + 清屏。

### 5. gfx24 报 no VBE/Bochs-dispi —— ✅ v2.20 已解决（见 §六）
VMware 没有 Bochs VBE dispi 设备（那是 QEMU -vga std 专属）；PCI 列表里的
`15AD:0405` 是 VMware SVGA II。原判定「先网络/音频后图形」被用户 2026-11-13
直接点名推翻：已实现 SVGA II 驱动（BAR0 索引/数据端口 + BAR1 LFB + BAR2 FIFO），
`vbe` / `vbe24` / `gfx24` 三条命令在 VMware 显卡上全部可用。

### 6. 声卡 "no SB16 detected" —— 已定性（预期行为）→ 拆为任务 #56
VMware 默认声卡是 ES1371（Ensoniq AudioPCI，PCI 列表里 `1274:1371`），不是 SB16
（QEMU 专属 ISA 设备）。报错本身正确；要 VMware 出声必须写 **ES1371 PCI 驱动**
（DMA 环形缓冲 + 采样率寄存器），接入现有 snd_play() 异步模型。QEMU 侧可用
`-device es1370` 同族验证。SB16 路径保留不动。

### 7. VMware 网卡 —— 拆为任务 #55（P1 核心项）
"我们必须做完备的网络"：VMware 默认网卡 e1000（Intel 82545EM `8086:100E`）。
- e1000 驱动：PCI 探测 → MMIO/IO 端口 → RX/TX 描述符环 → 中断 → 接入现有
  net 栈（http/dns/udp/ping）
- QEMU 用 `-device e1000,netdev=n0` 可同代码验证（不再依赖 pcnet）
- vmxnet3（15AD:07B0）可选，优先级低于 e1000

### 8. netdiag 伪就绪 + 提示误导 —— 拆为任务 #54（P0，小改）
VMware 里 `netdiag` 显示 `net_ready=1 mac=FF:FF:FF:FF`——**探测失败却报就绪，
这是 bug**（MAC 全 FF 说明寄存器读的是空总线）：
- NIC 探测失败必须 `net_ready=0`，netdiag/captest 的 snd/net 返回值要如实
- 探测到 `15AD:0405`（VMware SVGA）/`1274:1371`（ES1371）时判定运行在 VMware，
  网络提示改为"VMware: 将网卡设为 E1000，或使用未来的 e1000 驱动"；QEMU 下才
  提示 `-device pcnet`

### 9. adofai.ce 三项 —— 拆为任务 #60
① 屏幕闪烁：渲染双缓冲/脏区重绘 ② 开局 "3" "2" "1" "Go!" 倒计时字幕
③ 第一次点击后，准度条改为显示"上一次点击的误差量"（早/晚多少 ms）。

## 二、本次会话遗留（进行中）

| # | 任务 | 状态 |
|---|---|---|
| T50 | lang 语言设置命令（默认英文，持久化 /etc/lang，zh 翻译关键消息） | 待做 |
| T51 | Shell 交互：Ctrl+↑↓ 滚动回看、↑↓ 历史、语法高亮、←→ 光标编辑（块状闪烁光标） | 待做 |
| T52 | 用户密码交互输入（login/su/passwd 隐藏回显）+ useradd 交互式权限向导 | ✅ 本轮完成（见 §五） |

## 三、优先级建议

```
P0  #54 net 伪就绪修复（真 bug，一行级）+ T52 密码/权限向导（安全闭环）
P1  #55 e1000 驱动 → 完备网络；#56 ES1371 声卡；T51 Shell 交互增强
P2  #59 后台输出；#57 退出图形恢复调色板；T50 lang
P3  #58 VMware SVGA II 图形驱动；#60 adofai 打磨
```

2026-11-13 执行后：**P0 两项（#54 / T52）已完成**；**#57 / #59 / #58 / #60 已完成**；
**VMware SVGA II 图形驱动（原 #5 / P3）由 v2.20 提前完成**；
剩余 **#55 / #56 / T51 / T50** 未开始，其中 #55 是"完备网络"的关键路径。


## 四、VMware vs QEMU 设备对照表（实测结论）

| 设备 | QEMU | VMware 17.6.1 | CatOS 现状 |
|---|---|---|---|
| 显卡 | Bochs VBE dispi（-vga std） | VMware SVGA II（15AD:0405） | ✅ v2.20 双后端：Bochs 24bpp / SVGA II 32bpp XRGB 承载 RGB24 |
| 声卡 | SB16（ISA） | ES1371（1274:1371，PCI） | 仅 SB16 → VMware 无声 |
| 网卡 | pcnet（1022:2000） | e1000/vmxnet3（15AD:*） | 仅 pcnet → VMware 无网 |
| 磁盘 | IDE | IDE（vmdk 描述符） | ✅ 可用 |
| BIOS | SeaBIOS | VMware BIOS | ✅ 引导同源已实测 |

---

## 五、2026-11-13 执行进度（云主机）

### 5.1 环境结论（云主机构建链）

| 现象 | 根因 | 解决 |
|---|---|---|
| `extapp/build_ce.sh: 16: set: Illegal option -` | Windows 打包文件为 CRLF，`dash` 解析 `set -e\r` 失败 | 全量 `sed -i 's/\r$//'`（*.sh/*.c/*.h/*.s/*.py/*.ld/*.list/*.md） |
| `src/task.c:1972: invalid suffix "o111"` | `0o111` 是 C23 字面量，云主机 gcc 11 + `-std=c99` 不支持 | 改为标准八进制 `0111` |
| `Failed to get "write" lock`（已存在 img） | 容器文件系统锁，QEMU 5.2 默认加锁 | `-drive file=...,format=raw,if=ide,file.locking=off` |

云主机 binutils 2.38 的 `ld` 支持 `i386pe` 仿真、`objcopy` 支持 `pe-i386`，
因此 Windows 的 PE 链接流程（`pe.ld` + `objcopy -O binary`）在 Linux 上原样跑通，无需改构建脚本。

### 5.2 逐项状态

| # | 优先级 | 状态 | 本轮改动 | 验证 |
|---|---|---|---|---|
| #54 net 伪就绪 | P0 | ✅ 完成 | `net.c`：失败码（no-dev/bus-FF/mac/init）、CSR0 读 `FFFF/0000` 判 dead bus、五处失败点置 `net_ready=0`、新增 `net_env_vmware()`（扫 `15AD:0405/0790/07B0/0740`、`1274:1371`）；`shell.c`：`net_why_down()` 按失败码给原因，VMware→"NIC 设为 Intel E1000"，QEMU→`-device pcnet,netdev=n0`；`cmd_netdiag()` 增加 `fail=`/`env=`/`mac_state=`；`kernel.c` 启动提示环境感知 | **QEMU 实测通过**：`-net none` 下 `net: no usable NIC (no 1022:2000)` + PCI 列表 + QEMU 建议，`net_ready=0`（不再是伪 1） |
| T52 密码/权限向导 | P0 | ✅ 完成 | 新增 `kb_readline_masked()`（`*` 回显、退格擦一个星号、不支持 IME）；`login`/`su` 未给密码时交互提示 `Password:`；`passwd` 无密码参数时两次输入 + 比对 + 空密码拒绝（保留 `passwd NAME NEWPW` 脚本形式）；`useradd` 无 CAPS 参数时逐项 y/N 询问 **7 个能力位**（NET/KILL/FSADMIN/AUDIO/USERADM/DEVICE/SYS），结束时打印 `caps=0x…` | 编译通过；交互路径待实机点测 |
| #57 退出图形清屏 | P2 | ✅ 完成 | 审计确认 `be_text.c::text_enter()` 已完整恢复（16 色 DAC 重载、AC palette 0x00-0x0F 重写、plane 0/1/3 清零、sequencer reset 脉冲、video off/on、(0x00,0x00) 预填 0xB8000 破 QEMU per-cell 缓存条纹）；补上 `cmd_text()` 成功后 `vga_clear()` + 重置样式再提示 | 编译通过；图形↔文本往返视觉待实机点测 |
| #59 后台输出 | P2 | ✅ 完成 | `task_t` 增 `bg_mode` / `log_head` / `log_tail` / `log_lost` / 1 KB 环形 `log_buf`（spawn 复用槽位时清零）；`task.c` 新增 `task_log_append/pending/drain`；`SYS_WRITE` 控制台分支在 `bg_mode` 下改写入环形，`SYS_PUTC` 同步捕获，`SYS_CLS` 后台任务被忽略（不再擦前台屏）；`shell.c` 新增 `jobs`（TID/状态/名字/待读字节）与 `log TID` / `log -f TID`（跟随到任务退出、溢出丢字节提示、约 60 s 安全退出） | 编译通过；命令行为待实机点测 |
| #58 图形文字 | P3 | ✅ 完成（非 SVGA II 路线） | `be_gfx/be_vbe/be_vbe24` 的 ASCII 字形改为 **7×15 内缩**（格内留 1 px 字距 + 1 px 行距，等价 9×16 观感），不改 8 px 列网格 → 保住 CJK=2×ASCII 不变式与所有 pan/scroll 偏移；下划线移到 +15、删除线 +8→+10；三个后端 `draw_cursor` 由 1 px 线改为**下半块块状光标**；`render.c` 的 `paint()` 仅在 `blink_phase==0` 时绘制光标，`con_tick()` 每 0.5 s 翻转 → 光标闪烁且旧位置被整格重绘、无残留 | 编译通过；`CJK=2×ASCII` 逐像素回归断言**尚未补入 tests** |
| #60 adofai | P3 | ✅ 完成（三项） | ① 新增内核 syscall `SYS_GOTOXY`（slot 65，append-only，`ce_abi.h` 加 `gotoxy` 槽 + `con_set_cursor` 落地），adofai 改为**脏行重绘**（只刷 row1 HUD / row9 marker / row10 判断 / row11 误差），旧内核自动退回全屏重画；② 开局 **3-2-1-GO!**（各 1 s，GO 额外 0.5 s，期间时钟不前进、按键只排空不判定、ESC 可退出）；③ 判断行下方新增 `LAST ±N ms EARLY/LATE (pressed too soon/too late)`，Perfect/E/LPerfect/Early/Late 全部记录带符号误差 | 编译通过（`adofai.ce` image=11704, bss=0x4920, api_off=0x2db4）；实机手感待点测 |
| #55 e1000 | P1 | ⬜ 未开始 | — | — |
| #56 ES1371 | P1 | ⬜ 未开始 | — | — |
| T51 Shell 交互 | P1 | ⬜ 未开始 | — | — |
| T50 lang | P2 | ⬜ 未开始 | — | — |

### 5.3 本轮产物

| 文件 | 大小 | 说明 |
|---|---|---|
| `build/kernel.bin` | 733,684 B | 含本轮机/`#54/#57/#58/#59` 与 `SYS_GOTOXY`（原 725,396 B） |
| `build/kernel.pe` | 752,177 B（上一轮） | PE 中间产物 |
| `catos.iso` | 11,460,608 B | 新 kernel + 20 个 CE 应用 + node.ce |
| `catos_disk.img` | 67,108,864 B | 39 files，`adofai.ce` 已更新 |

QEMU 启动实测（云主机，QEMU 5.2.0）：
`-L <bios> -m 256 -no-reboot -net none -cdrom catos.iso -boot order=d
-drive file=catos_disk.img,format=raw,if=ide,file.locking=off -display none
-monitor tcp:127.0.0.1:4444,server,nowait` → 正常进 `root@catos:/$`。

### 5.4 本轮遗留（下一轮直接接手）

1. **#55 e1000 驱动**：PCI 探测 → BAR0 MMIO → RX/TX 描述符环 → 中断 → 接 `net_send/net_recv`；QEMU `-device e1000,netdev=n0` 验证（云主机无 `efi-e1000.rom`，需 `-netdev user -device e1000` 走非 UEFI 路径）。
2. **#56 ES1371**：`1274:1371` → PCI BAR → DMA 环形缓冲 + 采样率寄存器 → 接 `snd_play()`；QEMU `-device es1370` 同族验证；SB16 路径保留。
3. **T51 Shell 交互**：行编辑器（↑↓ 历史、←→ 光标编辑、Ctrl+↑↓ 回看、语法高亮）；块状光标闪烁已在 #58 就位，可直接复用。
4. **T50 lang**：`/etc/lang` 持久化 + `lang` 命令 + 关键消息翻译表。
5. **回归复跑**：`tests/qemu_regress.py`（需 `-L` wrapper 注入与 `--disk` 副本绕 write lock）、`user_regress.py`、`disk_regress.py`、`security_regress.py` 全绿，并补 `CJK=2×ASCII` 逐像素断言。
6. ~~**反向同步**：云主机改动尚未回写 `D:\CatOs`~~ → **2026-11-13 已完成**：
   `CatOs-v2.19-源码与文档.zip` + `CatOs-v2.19-镜像产物.zip` 从下载文件夹反向同步，
   提交 `3ed5cad`（v2.19 基线）；覆盖前的原文件备份在 `D:\_v219_backup\`。
7. **VMware 实机项**（#55/#56）只能在 QEMU 同族设备上验证，VMware 真机行为需用户复测；
   SVGA II 图形已在 QEMU `-vga vmware`（同 15AD:0405）逐像素验证，见 §六。

---

## 六、2026-11-13 Windows 侧：v2.20 VMware SVGA II 24 位真彩

用户反馈「下载文件夹有最新版。顺便做 VMware 24 色真彩支持。我输入 vbe 和 vbe24
都不行」。两条都是真问题：

| 症状 | 根因 | 处理 |
|---|---|---|
| `vbe24` 不识别 | shell 只注册了 `gfx24`，没有 `vbe24` 别名 | 分发链加 `vbe24` 别名 + help 文案 |
| `vbe` / `gfx24` 报 no VBE/Bochs-dispi | VMware 显卡是 `15AD:0405` SVGA II，没有 0x01CE dispi 端口；后端只有 Bochs 一条路 | 新增 `src/svga.c` + `include/svga.h`，按 PCI 身份选择后端 |
| QEMU `-vga vmware` 实测黑屏 | QEMU 的 vmware-svga 复用通用 VGA 核，**仍会应答 dispi ID**，旧代码因此走 Bochs 路径把像素写到不存在的 `0xE0000000` | 改为「PCI 上发现 SVGA II 就优先 SVGA」，dispi 只作兜底 |

实现要点：

- BAR0 I/O（index=base+0 / value=base+1）、BAR1 LFB、BAR2 FIFO；
  寄存器号对齐 VMware `svga_reg.h` / QEMU `vmware_vga.c`。
- SVGA II 是 32bpp XRGB 设备（DEPTH 读回 24，BPP 只有 32）。CatOS 保持
  **RGB24 公共绘图 ABI 不变**，按设备给的 `BYTES_PER_LINE` 与颜色掩码在
  运行时决定 pitch / 每像素字节数 / 通道偏移，第 4 字节写 0xFF。
- 启用 FIFO 后宿主停止 VGA dirty tracking，直写显存不会自动上屏；
  IRQ0 的 `svga_tick()` 以 25 Hz 推送全屏 `SVGA_CMD_UPDATE` 并写 SYNC。
- `paging.c::vbe_lfb_map()` 改为累积 PDE 区间，让 FB 与 FIFO 两个 BAR
  同时镜像进每个任务页目录（PTE 仍是 supervisor-only，#84 不破）。
- `extapp/gfx24.c` 不再硬编码 `pitch==2400`（改为 `pitch >= w*3`），
  否则 ring3 探针在 SVGA 上会拒绝绘图。
- 新命令 `svgainfo`；`display` 增 `graphics HW` / `last gfx24 hw` / `hw_bpp` / `stride`；
  `gfx_info` 追加三个只读选择器（BACKEND / HW_BPP / STRIDE，ABI append-only）。

验证（`tests/svga24_regress.py`，QEMU 11.1 `-vga vmware`，monitor 4467）：
**34 项全绿**，含 gfx24/vbe24 演示图逐像素（红/绿/蓝/白/渐变/8 色条）、
ring3 `gfx_rect`/`gfx_blit`、FIFO UPDATE 计数、`vbe` 真彩文本 RGB 断言。
QEMU `-vga std` 老路径 `gfx24_regress` 仍 0 failed；qemu 12/12、user 15/15、
security 17/17、disk 5/5、宿主 test_mkce 48 / test_cc 10 / ce_regress 11P /
test_app_list 24 全绿。
**真实 VMware Workstation 尚需用户开 VM 复测**（本机 VM 当前未运行）。

