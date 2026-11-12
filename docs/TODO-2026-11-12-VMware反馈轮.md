# CatOS 待办事项（2026-11-12，VMware 实测反馈轮）

来源：用户在 VMware Workstation 17.6.1 里实测 CatOS 的 9 点反馈 + 本次会话遗留。
现状基线：v2.18（LFN 已入库 `9ca51c1`），回归 user 15/15、disk 5/5、qemu 12/12、
security 17/17 全绿。

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

### 5. gfx24 报 no VBE/Bochs-dispi —— 已定性（预期行为）→ 待办 #58 后续
VMware 没有 Bochs VBE dispi 设备（那是 QEMU -vga std 专属）；PCI 列表里的
`15AD:0405` 是 VMware SVGA II。要 VMware 图形需新写 SVGA II 驱动（MMIO + FIFO），
列入 P2（先网络/音频后图形）。

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
| T52 | 用户密码交互输入（login/su/passwd 隐藏回显）+ useradd 交互式权限向导 | 待做 |

## 三、优先级建议

```
P0  #54 net 伪就绪修复（真 bug，一行级）+ T52 密码/权限向导（安全闭环）
P1  #55 e1000 驱动 → 完备网络；#56 ES1371 声卡；T51 Shell 交互增强
P2  #59 后台输出；#57 退出图形恢复调色板；T50 lang
P3  #58 VMware SVGA II 图形驱动；#60 adofai 打磨
```

## 四、VMware vs QEMU 设备对照表（实测结论）

| 设备 | QEMU | VMware 17.6.1 | CatOS 现状 |
|---|---|---|---|
| 显卡 | Bochs VBE dispi（-vga std） | VMware SVGA II（15AD:0405） | 仅 VBE → VMware 无图形 |
| 声卡 | SB16（ISA） | ES1371（1274:1371，PCI） | 仅 SB16 → VMware 无声 |
| 网卡 | pcnet（1022:2000） | e1000/vmxnet3（15AD:*） | 仅 pcnet → VMware 无网 |
| 磁盘 | IDE | IDE（vmdk 描述符） | ✅ 可用 |
| BIOS | SeaBIOS | VMware BIOS | ✅ 引导同源已实测 |
