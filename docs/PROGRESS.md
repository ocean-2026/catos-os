# CatOS 进度汇报（2026-11-11）

> 覆盖窗口：2026-11-07 合流之后至 2026-11-11（含 v2.12~v2.14）· 主线 `D:/CatOs` master
> 上一份汇报（2026-10-30 自举/JS 解释器轮）随 git 历史保留。
> 行动纲领见 `docs/ROADMAP-通用操作系统.md`；问题明细见 `docs/CatOS-内核问题清单.md`。

---

## 一、总览

| 阶段 | 状态 | 提交 |
|---|---|---|
| 现项目 × 原项目合并取精华（30+ 提交）+ master 合流 | ✅ 完成 | `6945860` |
| SB16 声卡驱动 + 合流遗留修复 | ✅ 完成 | `cc631c8` |
| 用户指针跨页校验 P0（#78） | ✅ 完成 | `a5da8e3` |
| 800x600x24 真彩后端 + 用户绘图 ABI（#83~#85） | ✅ 完成 | `15cf106`（v2.12） |
| 消除劣根性第一批：syscall 边界/诊断/fsck/单一来源（#86~#89） | ✅ 完成 | `62cd529` |
| 应用与内核解耦（#90） | ✅ 完成 | `4fb3318` |
| 多用户 + 能力权限 + 初始 root（#91） | ✅ 完成 | `febc9b8`（v2.13） |
| ot / adofai 老项目收编 + pollkey + shell 文件四件套（#92~#95） | ✅ 完成 | `443c067`（v2.14） |
| GitHub 推送 | ❌ 未做 | 全部提交仅本地 |

---

## 二、本轮（11-10 ~ 11-11）完成明细

### 2.1 #86~#89 可靠性第一批（`62cd529`）
- **#86**：SYS_WRITE / SYS_FILE_WRITE / SYS_PIPE_WRITE 三处"校验后裸解引用"全部改为
  `copy_from_user()` 内核快照（分段/分块/一次性），`vprobe` 回归扩到 10/10。
- **#87**：新增 `diag.c`——致命故障记录向量/ring/任务/eip/CR2，shell `faultinfo` 可事后归因，
  `panic()` 统一停机入口；`diag_regress.py` 4/4。
- **#88**：`fat_verify_mirror()` mount 时校验并修复 FAT#2，shell `fsck` 手动重跑。
- **#89**：`build/apps.list` 成为应用清单单一来源，`test_app_list.py` 守卫。

### 2.2 #90 应用与内核解耦（`4fb3318`）
- `build/mkdisk.py` 宿主侧逐字节复刻 `fat_mkfs()` 布局，生成预装 20 应用的
  `catos_disk.img`；`build.sh disk` 一键产出。**磁盘成为应用真实来源，kernel.bin 只剩
  bootstrap fallback 语义**——改一个应用不再需要重编内核。
- 免 `format` 直接挂载；`disk_regress.py` 5/5。

### 2.3 #91 多用户与细粒度权限（`febc9b8`，v2.13）
- `src/user.c`：持久化 `/etc/passwd`（name:uid:gid:home:shell:caps:passhash）与
  `/etc/acl`（path:uid:gid:mode，12 位 Unix 语义）；种子 `root`（uid 0，CAP_ALL，
  **初始用户**）+ `user`（uid 100）。
- **能力位** CAP_NET/KILL/FSADMIN/AUDIO/USERADM/DEVICE/SYS 在**内核 syscall 分发处**
  强制（网络 25-32、音频 57-60 槽缺位返回 -13）；`task_kill` 改 root|属主|CAP_KILL。
- shell 新命令：whoami/id/lsuser/caps/login/su/useradd/userdel/passwd/chcaps/chmod/chown，
  提示符 `user@catos:/$`；`format`/`fsck` 要 CAP_FSADMIN，`sudo`/用户管理要 CAP_USERADM。
- **零新增 syscall 槽**，64 槽 ABI 不动。`user_regress.py` 15/15（端口 4465）。

### 2.4 #92~#95 ot / adofai 老项目收编（`443c067`，v2.14）
- 下载目录的 `catos-ot-game-2026-11-11.zip`（ot 工具链 + adofai 音游，2026-10 用户态线）
  按"内核已有的从 ot 去掉、没有但有价值的内核态实现"收编。
- **#92**：CE ABI v2.14 追加 slot 64 `pollkey()`（非阻塞按键）；adofai 的
  "屏蔽 IRQ1 + 直读 PS/2 端口" ring0 hack 全删。
- **#93**：shell 新增 `cat`/`rm`/`cp`/`mv`（权限走 ACL，cp 归属创建者 0644）；
  ot 命令表中 30 个与 shell 重复的命令移除。
- **#94**：ot 收编为 `extapp/ocntools.c` → `/bin/ocntools.ce`（脚本引擎/文件管理器/
  十六进制编辑/搜索/版本控制/镜像检查保留），修了 `.text.ce_entry`、全局 `__ce_api`、
  NULL api 解引用三处遗留。
- **#95**：adofai 收编为 `extapp/adofai.c` → `/bin/adofai.ce`，节拍音效走 v2.11
  `snd_tone`（无声卡静音不报错）；关卡 DEMO1/DEMO2.TAD 随镜像。
- 新回归套件 `tests/ot_regress.py` 16/16，全套无回归（见问题清单 17.1）。

---

## 三、回归总账（#95 后同 ISO 全套）

```
实机: ot_regress 16/16  qemu_regress 12/12  user_regress 15/15  diag_regress 4/4
      disk_regress 5/5  vprobe_regress 10/10  snd_regress 6/6  gfx24_regress 0 failed
      heap_regress 4/4  go_regress 7/7  qa_probe_final exit=0
宿主: test_mkce 48  test_cc 10  test_app_list 23 apps  ce_regress 11P/16S/0F
```

## 四、已知遗留（详见问题清单）

- spawn 不查 `PERM_EXEC`；口令哈希为 djb2（非密码学）；单 gid 无多组。
- 12.2 观察：syscall 内 static 缓冲靠 `g_in_syscall` 非抢占保证安全，将来允许
  syscall 让出 CPU 时须分槽。
- 判定重写级、不做：恒等映射/独立地址空间、SMP/抢占、内存动态化（理由见 ROADMAP 第三节）。

## 五、下一步（对应 ROADMAP 第四节）

P1：spawn 执行权限、多组支持、口令哈希升级、shell 作业控制（`&`/`jobs`/`fg`）。
P2：用户态 libc 雏形、自举环闭环（cc_self 编译自身）、LFN 长文件名、目录操作命令。

---

*本文件随每轮交付滚动更新；历史汇报见 git 历史（`git log -- docs/PROGRESS.md`）。*

---

## 六、截图审计轮（#97~#102，2026-11-12）

12 张 QEMU 实机截图审计出 6 个问题，全部修复（详见问题清单第十九节）：

- **#97（P0）** ACL 路径大小写绕过：`fs_canon_path()` 规范化 + 大小写不敏感比较，
  `/BIN/X.CE`、`./x.CE` 等变体不再绕过 chmod。
- **#98** `cmd_chmod`/spawn 未初始化变量（栈垃圾写入 ACL）。
- **#99（P0，流程）** 构建链骗测：patch_work_iso.py 拒绝打包陈旧 kernel；
  `build.sh iso` 补 `build_apps` 依赖。
- **#100** vdiag 残影/调色板污染（glyphA 一次性输出 + AC 重触发）。
- **#101** CJK 全角标点字形 6961 个（Unifont A1/A3 行 + punctest.ce 探针）。
- **#102** build.sh iso 目标依赖修复。
- **澄清（非 bug）**：磁盘镜像 MBR 无引导代码，引导介质是 catos.iso
  （GRUB isohybrid 双格式）。VMware 引导工件见 `vmware/`（vmx + vmdk
  描述符 + README），isohybrid 当 IDE 硬盘引导已实测到 shell。

### 回归总账（2026-11-12 同 ISO 全套）

```
宿主: test_mkce 48  test_cc 10  test_app_list 24 apps  ce_regress 11P/17S/0F
实机: qemu_regress 12/12  user_regress 15/15  ot_regress ALL  diag_regress 4/4
      disk_regress 5/5  heap_regress 4/4  go_regress 7/7  vprobe_regress 10/10
      snd_regress 6/6  gfx24_regress 0 failed  security_regress 17/17
```

### 已知遗留

- 口令哈希 djb2（非密码学）、单 gid 无多组（ROADMAP P1 不变）。
- VMware 为 SeaBIOS 同源验证 + 配置工件，未经 VMware Workstation 本机实测
  （本机未装）；待用户在 VMware 里按 vmware/README.md 验证。

## 七、开机体验轮（2026-11-12，v2.17）

- **#103（bug）** 退格 `?` 残影：be_text 把空格子（cp==0）画成 '?'。
  改为仅可打印 ASCII 显示原字符、其余画空格。QEMU 复现→修复→验证
  （输 cba + 3 次退格屏幕干净，tests/qemu_shots/bs2.png）。
- **自启动目录（特性）**：开机不再打印硬编码 banner，改为顺序执行
  `/etc/autoexec/` 下脚本（每行一条 shell 命令，# 注释/空行跳过）；
  空卷自动播种 banner.sh（echo 行复刻原 banner）。新命令
  `autostart` 可随时重跑。shell 分发链重构为 shell_exec() 供交互与
  脚本共用。**注意 8.3 命名限制**：目录名用 autoexec 而非 autostart
  （9 字符 stem 被无 LFN 的 fs_mkdir 拒绝——曾因此静默失效）。
- **新开机 logo**：about 的 ASCII 大字换成块状 CatOS。
- 回归：security 17/17、user 15/15、qemu 12/12（autostart 实测截图
  tests/qemu_shots/fin0.png）。

## 八、v2.19 回流 + v2.20 VMware SVGA II 24 位真彩（2026-11-13）

### 8.1 v2.19 反向同步

云主机（Linux `vm`）完成的 #54 / #57 / #58 / #59 / T52 / #60 与
`SYS_GOTOXY`（槽 65），经下载文件夹两个 zip 反向同步回 `D:\CatOs`，
提交 `3ed5cad`（源码 + 文档 + kernel.bin + 全部 .ce 产物）。
覆盖前原文件备份在 `D:\_v219_backup\`。详见
`docs/TODO-2026-11-12-VMware反馈轮.md` §五/§六。

### 8.2 v2.20 新增

- **VMware SVGA II 驱动**（`src/svga.c` / `include/svga.h`）：PCI `15AD:0405`
  探测、BAR0 索引/数据端口、BAR1 LFB、BAR2 FIFO、模式设置、
  25 Hz `SVGA_CMD_UPDATE`（IRQ0 钩子）、`svgainfo` 诊断命令。
- **双后端**：`be_vbe` / `be_vbe24` 按 PCI 身份在 Bochs dispi 与 SVGA II
  之间选择。SVGA II 是 32bpp XRGB，但 CatOS 的 RGB24 绘图 ABI 不变，
  pitch / 每像素字节数 / 通道字节序全部运行时决定。
- **命令**：`vbe`、`vbe24`（新别名）、`gfx24` 在 VMware 显卡上全部可用；
  `display` 增加 `graphics HW` / `last gfx24 hw` / `hw_bpp` / `stride`。
- **修复 #104~#106**：无图形、`vbe24` 不存在、QEMU `-vga vmware` 因
  dispi 端口误判导致黑屏。详见问题清单第二十一节。
- **连带**：`paging.c` LFB PDE 区间取并集；`extapp/gfx24.c` 不再硬编码
  `pitch==2400`；`gfx_info` 追加 BACKEND/HW_BPP/STRIDE 选择器（append-only）。

### 8.3 回归总账（2026-11-13 同 ISO）

```
宿主: test_mkce 48  test_cc 10  test_app_list 24 apps  ce_regress 11P/17S/0F
实机(-vga std): qemu 12/12  user 15/15  security 17/17  disk 5/5
                gfx24_regress 0 failed
实机(-vga vmware): svga24_regress 34/34 PASS
                   (gfx24/vbe24 逐像素 + ring3 rect/blit + vbe 真彩文本
                    + FIFO UPDATE + text 往返)
```

真实 VMware Workstation 17.6.1 尚需用户开机复测；本机 VM 当前未运行，
`vmware/CatOS.vmx` 光驱已指向新 `kernel_v25/catos.iso`，数据盘上的
`/bin/gfx24.ce` 已原地更新（改前备份 `catos_disk.img.bak-v219`）。

## 九、v2.21 外设与交互 + 光标/黑屏修复轮（2026-11-13）

### 9.1 v2.21 新增

- **e1000 网卡驱动**（`src/e1000.c`）：PCI 探测 → BAR0 MMIO → RX/TX 描述符环
  → 中断 → 接 `net_send/net_recv`；`netdiag` 可核对后端与收发计数。
- **ES1371 声卡驱动**（`src/es1371.c`）：PCI BAR + DMA 环形缓冲 + 采样率寄存器
  → 接 `snd_play()`；与 SB16 并存，探测时按 PCI 身份择一（`sndinfo` 可查）。
- **T51 shell 行编辑器**（`src/keyboard.c`）：↑↓ 历史、←→ 光标编辑、Ctrl+↑↓
  回看滚动、语法高亮；`render.h` 增 `con_get_cursor()` 与滚动回看接口。
- **lang 命令**（`src/lang.c`）：`/etc/lang` 持久化 + 关键消息翻译表。
- **CE ABI**：`ce_abi.h` / `intr.h` / `keyboard.h` / `net.h` / `snd.h` 同步扩口。

### 9.2 本轮修的缺陷（#107~#111，详见问题清单第二十二节）

- **#107 VMware `vbe` 黑屏**：`paging.c` LFB/MMIO 映射加 **PCD（不可缓存）**位。
- **#108 光标不闪 + SVGA 不刷新**：`cpu.s` 把 IRQ0 单独接到 `sched_timer_tick`，
  `con_tick()` / `svga_tick()` 挂在 `isr_irq` 上等于**死代码**；改为在
  `sched_timer_tick()` 里直接调用。光标恢复闪烁，SVGA FIFO 真的在推 `UPDATE`。
- **#109 光标样式**：三个图形后端由 8×8 实心块改为**格子末行 1 px 下划线**；
  有字格子由 `render.c::paint()` 画**背景反色**；文本模式硬件光标 13–14 → 14–15。
- **#110 残光标**：`con_set_cursor()` 保留 `dcx/dcy`；`scroll_one()` 滚动前先擦光标。
- **#111 回归断言过期**：`heap_regress` 的 `ls /bin` 改断言 LFN 长名。

### 9.3 新增逐像素回归 `tests/cjk_width_regress.py`（monitor 4493）

CJK = 2×ASCII 的**逐像素**断言（下划线连续段 = 精确格子标尺）：

| 对象 | 组成 | 期望 | 实测 |
|---|---|---|---|
| 纯 ASCII 行 | 49 格 × 8 px | 392 px | 392 px ✅ |
| 混排行 | 12 ASCII × 8 + 14 CJK × 16 | 320 px | 320 px ✅ |

⇒ ASCII 步进 8 px，CJK 步进 **16 px = 2×ASCII**（1 格宽会是 224 px，3 格宽 416 px，
两者都会判失败）。补齐 TODO 轮 §5.4 的"CJK=2×ASCII 逐像素断言"。

### 9.4 回归总账（2026-11-13 同 ISO，LF 归一后重跑）

```
宿主: test_mkce 48  test_cc 10  test_app_list 24  ce_regress 11P/17S/0F
实机(-vga std): qemu 12/12  user 15/15  security 17/17  disk 5/5  diag 4/4
                heap 4/4  go 7/7  snd 6/6  e1000 0F  es1371 0F
                gfx24_regress 0F  svga24_regress 0F  cjk_width_regress 0F
```

**行尾归一**：v2.19 反向同步带进来一批 CRLF 文件，`core.autocrlf=false` 下
会把整文件改写成 CRLF blob。本轮把 7 个"工作区 CRLF / HEAD LF"的文件归一回 LF
（`render.h` `be_gfx.c` `be_vbe.c` `intr.c` `keyboard.c` `render.c` `heap_regress.py`），
提交差异从 ~2900 行降到 **689 插入 / 109 删除**，且归一前后 `kernel.bin`
大小完全一致（6,662,308 字节）。

**真实 VMware Workstation 仍需用户开机复测**（#107 黑屏、#108 刷新、#109/#110 光标）。

## 十、v2.22 口令加固轮（2026-11-14 跨夜）

- **加盐迭代口令哈希**（`src/user.c`）：新 /etc/passwd 第 7 字段格式
  `$S$<8hex salt>$<8hex digest>`；FNV-1a + fmix32 雪崩，8192 轮迭代，
  每记录独立随机盐（timer_ticks 混地址）。同口令不同用户 → 不同盐。
  明确非密码学边界（内核无密码学原语），目标是抵御离线彩虹表/秒级爆破。
- **旧格式兼容 + 透明升级**：legacy 裸 djb2 记录仍可验证，登录成功后
  原地升级为加盐格式（users_dirty → 下次持久化写盘）；legacy "0"（无口令）
  在 user_db_init 归一为空串。
- **新回归 `tests/pw_regress.py`**：12 步（加盐落盘格式、错误口令/大小写
  变体拒绝、正确口令接受、同口令不同盐、无致命故障）。
- **测试基架 Linux/QEMU6.2 移植**：qemu_driver/qemu_regress 等 6 个驱动
  支持 PATH 查找 qemu-system-i386；镜像统一暂存 /tmp（hpvs_fs 无文件锁）；
  `xp` 地址正则放宽到 16 位；type_text 补 `$` 等按键映射。
- **回归总账（2026-11-14，同 ISO）**：宿主 test_mkce 48、test_cc 10、
  test_app_list 24；实机 qemu 12/12、security 17/17、user 15/15、diag 4/4、
  pw 12/12。

## 十一、v2.23 shell 环境变量轮（2026-11-14 跨夜）

- **env 模块**（`src/shell.c`，无新 syscall，ABI 不动）：`export K=V` /
  `setenv` / `env` / `unset K`；`$K`、`${K}` 在键入行与 /etc/autostart
  脚本行统一展开（`$$` 字面量，未知变量展开为空，sh 语义）；同名单上限
  16 项、key<16、val<64，表满/超长明确报错。
- **$PATH 驱动裸名查找**：`run_from_path` 第 2 段由硬编码 /bin 改为解析
  `$PATH`（冒号分隔、按序、容忍空段）；首启 `env_init()` 落盘默认
  `PATH=/bin`。
- **持久化 /etc/env**（KEY=VALUE，随改随存，风格同 /etc/lang）；
  中英文警告消息走 lang 翻译表（append-only L_ENV_NOSAVE）。
- **CE 应用暂不继承 env**：传环境块需 append-only ABI 新槽，列入待办。
- **新回归 `tests/env_regress.py`**：13 步全过（含 PATH 重定向查找、
  变量删除、持久化、无致命故障）。
- **回归总账（2026-11-14，同 ISO）**：qemu 12/12、security 17/17、
  user 15/15、pw 12/12、env 13/13；宿主 test_mkce 48、test_app_list 24。

## 十二、v2.24 递归文件操作轮（2026-11-14 跨夜）

- **fs 层新原语**：`fs_is_dir()`、`fs_rmdir()`（仅删空目录，`.`/`..` 之外的
  条目存在即拒绝，root 永不可删）。
- **shell 三件套对齐现代 OS**：`mkdir -p`（逐级创建、容忍已存在）、
  `cp -r`（目录树复制，深度上限 6，内容 16KiB 分块）、`rm -r`（递归清空
  + 删目录，拒绝 `/`）。裸 `rm 目录` 明确提示需 -r。
- **#112 修复**：`perm_write_on_parent()` —— mkdir / cp -r 落点父目录必须
  有 PERM_WRITE，非 root 用户再也不能在 /etc、/bin 下建目录。
- **#113 修复**：新增 `acl_unset()`，rm / rm -r / mv 成功删除后清理 ACL 条目，
  杜绝"重建同名文件继承旧权限"。
- **#114 修复**：副本 ACL 改 0755，与"无条目"默认一致，`cp` 后的 .ce 可直接 run。
- **新回归 `tests/fsrec_regress.py`**：12 步全过（嵌套创建、树复制+内容核验、
  树删除、误用防护、悬空 ACL、越权 mkdir、无致命故障）。
- **回归总账（2026-11-14，同 ISO）**：qemu 12/12、disk 5/5、security 17/17、
  user 15/15、pw 12/12、env 13/13、fsrec 12/12。

## 十三、v2.25 图形命令行增强轮（脏单元格 / 滚动锚定 / 字号三档）

- **字符单位脏矩形（v2.25 核心）**：render.c 维护 painted[][]（屏幕真实像素的
  单元格镜像），paint() 逐字节比较，相同内容零显存写入；新增选择器
  CON_INFO_CELLS_DRAWN=16 / CELLS_SKIPPED=17（display 显示计数，实测
  skipped 55→717→1629 持续增长）。painted 随 scroll 同步平移，模式切换/
  清屏/scale 切换时整体失效。
- **滚动锚定 #7**：con_scroll_anchor() —— shell 每次打印提示符前把输入行
  滚回屏幕中间行（带 scrollback 归档），长输出后提示符始终居中。
- **字号三档 #2**：vbe24 后端 set_scale(0/1/2) = 8x8(100x75)/8x16(100x37)/
  16x32(50x18)；ASCII 字形本源 8x8，密集模式取消拉伸、大号模式 2x 放大；
  CJK 恒为 2列x1行（密集模式 16x16 纵向压缩为 16x8），WIDE 不变式保住。
  shell 新命令 `size compact|normal|large`，选择持久化 /etc/size，
  重新进入 vbe24 时自动恢复；CON_INFO_CELL_SCALE=18 / CELL_W/H 动态化。
  reflow 复用既有机制，CON_MAX_ROWS 40→80。
- **测试**：tests/size_regress.py（文本模式计数断言 + vbe24 像素绿带断言，
  端口 4492）。normal-band / dirty-skip 等核心检查通过，compact/large
  三档字形截图经像素验证正确。
- **待查（记 #115）**：偶发 guest 崩溃（QEMU -no-reboot 退出，monitor 断管），
  在 size 回归长流程中非确定性出现，单步手动复现未果；下一步加串口日志
  或 panic 截屏定位。

## 十四、v2.25c 细粒度权限 + 全屏用户管理 UI（需求 4）

- **CAP 六细拆（append-only，宽位仍有效，各门接受 宽位 OR 细位）**：
  CAP_FSADMIN→CAP_FORMAT(0x0080)+CAP_FSCK(0x0100)；CAP_DEVICE→CAP_RAWIO(0x0200)
  +CAP_HWINFO(0x0400)；CAP_SYS→CAP_REBOOT(0x0800)+CAP_DIAG(0x1000)。
  format/fsck/reboot 门已切换到 宽 OR 细；`reboot` 由无门控改为需要
  CAP_SYS 或 CAP_REBOOT（root 例外）。
- **预设用户模板**：admin / operator / developer / viewer / guest；
  useradd 向导首选预设，`useredit` 内 R 键应用预设。
- **全屏接管 UI `useredit`**：con_screen_push()/pop() 整屏快照恢复；
  用户列表（name/uid/gid/caps hex/密码有无）+ 方向键选择 + N 新建 /
  E 细粒度 caps 逐位开关 / P 掩码改密 / D 删除 / Q 退出（退出后
  shell 界面逐字恢复）。需 CAP_USERADM。
- **修复**：user_preset_caps 预设比较条件写反（useq==0 误配 admin →
  caps=0xFFFFFFFF），实机捕获后修正。
- **回归**：tests/caps_regress.py 12 步全过（三命令拒绝、预设 0x400、
  细位放行、TUI 往返、屏幕逐字恢复、无致命故障）。

## 十五、v2.26 USB 支持（需求 5，本批 7 项全部完成）

- **UHCI 主机控制器驱动**（src/usb_uhci.c，~600 行）：
  PCI 扫描 8086:7020/7112（PIIX3/PIIX4 UHCI，含 function 扫描——QEMU PIIX3
  UHCI 在 dev1.fn2）；BAR4 I/O 基址；HCRESET/GRESET 复位序列；1024 项
  1ms 帧表（固定物理区 0x560000，与 es1371/snd DMA 区错开）→ QH → TD。
- **完整 USB 枚举**：GET_DESCRIPTOR(8B) → SET_ADDRESS(1) → SET_CONFIGURA
  TION(1)，控制传输经 SETUP/IN/OUT 三 TD 深度链，轮询收割。
- **HID 键盘**：中断 IN（addr1 ep1 8B boot report）10ms 轮询收割；
  make/break 按 6 槽报告比较；修饰键换档合成 set-1 0x2A/0xAA 等；
  HID usage→i8042 scancode set 1 翻译表（字母三段、数字、F1-F12、
  方向键、Home/End/PgUp/PgDn/Ins/Del），进 kb 统一队列——IME、
  scrollback、rl 编辑器全部无需改动即可用。
- **`usb` shell 命令**：控制器/端口/速度/报告数/错误数；`usb scan` 热插入。
- **调试攻坚记录（全部实机捕获）**：
  1. 只扫 fn=0 漏掉 PIIX3 UHCI（fn2）；
  2. FRBASEADD 是 32 位寄存器，outw 只写低 16 位 → 帧表基址=0；
  3. intr.c isr_irq case 0 是死代码，IRQ0 真入口在 cpu.s _irq_0 →
     task.c sched_timer_tick——usb_poll 原先永远不被调用；
  4. UHCI 控制器会推进 QH element pointer（TD 完成后 QEMU 把 TD[0]
     即 TERM 写进 QH[1]），每次重排 TD 必须同时把 QH[1] 指回 TD；
  5. TD 未武装时 TD[1]=0 被误判为"完成 1 字节"，产生幽灵报告；
  6. SET_ADDRESS 发两次且第二次用错设备地址（addr=1 发给 addr0 设备）；
  7. QEMU `-usbdevice keyboard` 存在时 sendkey 输入只走 USB 设备，
     PS/2 收不到——USB 路径是唯一输入通道（stale 内核验证）。
- **回归**：fsrec 12/12、caps 12/12、user 15/15 全绿；实机验证
  `echo hello-from-usb-keyboard` 全 USB 路径完整回显，reports 递增、
  errors=0。截图 tests/qemu_shots/usb_keyboard.png。

## 十六、v2.27 IME 候选条宽度自适应

- vbe24 large 档（50x18，16x32 cell）实机验证 IME：候选条正确跟随输入
  行、选字 commit（"中"）正确上屏。
- 修复：窄档候选条右缘截断（第 9 候选被切半、分页提示丢失）。现按
  con_cols() 计算每个候选占宽（数字 + 汉字 2 cells + 空格），放不下的
  候选主动跳过（= 翻页仍可达），80 列以上才追加 "(-/= page, esc)" 尾注。
- 回归：fsrec 12/12；normal 80 列候选条完整（9 候选 + 尾注）；large
  50 列完整（8 候选，无截断）。

## 十七、v2.28 TAB 补全 + Ctrl+C 行取消

- **Tab 命令补全**：kb_readline 挂补全钩子（kb_set_completer）；shell 侧
  sh_complete：光标位于命令 token 时对 71 个命令名前缀匹配——唯一命中
  直接补全 + 尾空格；多命中把候选列表画到输入行下一行反白条（不动
  行缓冲）。
- **Tab 文件/目录补全**：光标在参数 token 时用 fs_enum_entries（新增，
  含目录、目录名带 '/' 尾）前缀匹配；含 '"' 或路径中已有 '/' 的复杂
  情形暂不处理（保守 return 0）。
- **Ctrl+C 取消行**：kb_readline 遇 ^C（0x03）清行返回 2，shell 主循环
  打印 "^C" 重开提示符——与 bash 习惯一致。
- **根因教训**：fs_enum_files 跳过目录（FAT 目录过滤），补全需新的
  fs_enum_entries；测试盘上 zz_demo 实际不存在导致两轮误判"补全失效"。
- 回归：fsrec 12/12、caps 12/12、user 15/15 全绿。截图
  tests/qemu_shots/tab_complete.png。

## 十八、v2.30 USB 热插拔自动识别

- **缺口**：-usbdevice keyboard 启动时正常，但运行中插入的 USB 键盘
  （QEMU monitor device_add usb-kbd）导致输入全丢：kbd_port=0 时
  usb_poll 直接返回，新设备永远不会被发现，而 QEMU 的 sendkey 又被
  USB 设备独占——死锁。
- **修复（两段式）**：usb_poll（tick 上下文，不能跑带忙等的 attach）
  每 ~1s 探测各端口 connect status，命中只置 pending_attach；
  usb_service()（新接口）在任务上下文消费 pending 执行完整
  reset+enumerate+arm。调用点：kb_next_scancode / kb_next_scancode_timed
  的等待循环——同时把阻塞等待改为 2-tick 轮询（task_block 在无键生产者
  时永不唤醒，usb_service 永远没机会跑）。
- **实机验证**：-usb 启动 → device_add usb-kbd → 首键触发 attach
  （串口 breadcrumb：probe hit → service attach → attached）→ 后续输入
  完整回显。截图 tests/qemu_shots/usb_hotplug.png。
- 对抗测试记录：模式切换 churn（gfx24/size compact|normal|large/text 确认）
  3 轮 6/6 过；shell 输入 fuzz 60 轮无崩溃；并发后台任务 + scrollback
  深分页往返 + ps 正常；fsrec/caps/user 全绿。

## 十九、v2.31 history 命令

- keyboard 暴露 kb_hist_count/kb_hist_get（0=最老，环形栈换算），shell 新增
  `history` 命令：编号列出本次会话命令历史（与 Up/Down 共用 rl_hist，
  连续重复自动去重）。实机验证 tests/qemu_shots/history_cmd.png。
- fsrec/caps/user 回归全绿。

## 二十、v2.32 无盘 format 死循环修复（#116）

- 对抗测试发现：无 -drive 硬盘时 `format` y 确认后假死 20s+。
- 根因：fat_mkfs 在 ata_identify 失败时虚构 64MiB 卷继续写，每个扇区
  写烧满 ata 层 10M 迭代超时，累计分钟级。
- 修复：fat_mkfs→int（-1 无盘）、fs_format→int、format 命令与 boot
  自动 format 路径报错并继续。带盘 format 2s 正常，无盘立即报错。
- fsrec/caps/user 回归全绿。

## 二十一、v2.33 alias/unalias 命令

- bash 风格别名：alias name=value 设置（同名单写覆盖）、alias 列出全部/
  单条、unalias 删除；主循环对命令词做单层展开（value + 原参数拼接后
  重新分词进 shell_exec），8 条容量。
- 实机验证：alias ll='ls -l' → ll / 正确展开执行；unalias 后 ll 变
  unknown command。截图 tests/qemu_shots/alias_cmd.png。
- 对抗测试：编辑器 IME 全链路（F2 开启 → zhong 候选条 → 选"中"上屏 →
  ^S 保存 4 字节 → gfx24 下 cat 正确显示中文）✓。text 模式 VGA 字库
  无中文字形（cat 中文无输出）为硬件限制，记问题清单 #117 低优先级。
- fsrec/caps/user 回归全绿。

## 二十二、v2.34 df 命令 + #118 簇上界修复 + ls -l

- `df`：FAT 卷用量表（total/used/free/use%），fat_free_clusters 扫描
  FAT 空表项；无卷时报错。
- **#118（对抗测试发现）**：fat_alloc/free_clusters 用 max_clus（FAT 表
  容量）当数据簇上界——小卷上 max_clus 可达数据区 2 倍，可能分配越过
  数据区的簇（卷尾损坏风险），df 也因此 free>total。加 clus_hi()=
  vol_clusters+2 统一替换全部 17 处数据簇遍历上界。
- `ls -l`：长格式（perms/size/name），支持目录与单文件；`ls -l 文件`
  不再报 not a directory。
- 清理 keyboard.c/intr.c 缺失 include 的隐式声明警告。
- 实机验证：df 生命周期（写 40 行文件 used 274→275K）✓；ls -l 目录/
  单文件 ✓（-rw-r--r-- 312 hello.ce）。回归 fsrec/caps/user/qemu 全绿。

## 二十三、v2.35 PS1 自定义提示符

- prompt() 支持 PS1 环境变量：\u=用户 \h=主机 \w=当前目录 \$=提示符
  （root 显 #）；未设置时默认提示符不变。export PS1=... 设置、
  unset PS1 恢复。
- 测试基建：qemu_driver.type_text 补全 20+ 特殊字符映射
  （backslash/@/#+括号等），此前反斜杠与多数符号输入直接丢失。
- 实机验证：PS1=\u@\h:\w\$ 后提示符 root@catos:/#；unset 恢复默认。
- fsrec/user/qemu 回归全绿。

## 二十四、v2.35 后对抗测试补充

- 强化 churn 4/4：vbe24 三档字号 + vbe(16bit) + text 确认切换，无崩溃。
- alias+PS1 组合 fuzz：alias 展开进入图形模式、PS1 残留 unset 恢复 ✓。
- 编辑器压力轮 5/5：保存/重开/追加循环，含中文文件名、带空格文件名、
  40 字符超长路径正确拒绝（路径限制按设计生效）。
- **环境事件**：QEMU 二进制中途消失（Ubuntu 环境重置），表现为回归
  FATAL——重装 qemu-system-x86 后全套回归复测全绿，确认非内核回归。

## 二十五、v2.38 P0 4GB 内存支持（跨夜分片）

- **设计权衡（务实落地，未做内存动态化重构）**：沿用既有“内核虚拟地址
  = 物理地址”恒等映射假设，把 PSE 恒等映射范围从固定低 64MiB 扩展到
  multiboot mem_upper 实际报告的 RAM 上界，封顶 4GiB；帧分配器位图按
  4GiB 上限静态预留（32768 字 / 128KiB BSS），只管理 frames_base..mem_top。
  不引入每帧内核窗口、不取消恒等映射（ROADMAP 诚实清单 #1/#3 的重构项
  仍 deferred）。
- **32 位边界处理**：KB→字节用 uint64 中间量换算，避免 mem_upper<<10
  在近 4GiB 回绕；4GiB 本身不可表示，上界封顶 0xFFFFF000。
- **探测链路排查**：截断点共两处——hw.c HW_MEM_TOTAL_KB 钳到 63MiB、
  paging.c top 钳到 64MiB；均已去除。shell `mem` 命令直读 mbi 本来就如实，
  仅补打 PMM used/free 帧。
- **已知限制（遗留）**：① multiboot mem_upper 只统计 1MiB 以上的连续低
  内存，PCI hole 之上的第二段 RAM（mmap 表区域）本内核未解析，大内存
  客户机显示总量仍小于 `-m` 标称值，属如实反映；② 真 4GiB 客户机可能把
  PCI BAR 放进最后 4GiB 块，当前恒等映射未避让 MMIO，需后续内核窗口方案；
  ③ 本机 cgroup 内存上限 4GiB，无法实跑 `-m 4096`，最大验证档 3840MiB
  （客户机报告低内存 ~3071MiB，PCI hole 所致）。
- **遗留项 #122/#123**：#123 ata_read_sectors DRQ 失败改为返回 -1（与写
  路径对称）；#122 export/unset 加管理员闸门，/etc/size 落盘需管理员
  （会话内仍可临时切字号）。详见 docs/night-P0-report.md。

## 二十六、v2.47 shell did-you-mean 一键确认（y 直跑）

- **功能**：主提示符下 unknown-command 给出最近邻建议后，缓存该猜测命令（命令名 + 原始参数）；下一行裸 `y`/`Y` 即按与直接输入相同的 `shell_exec` 路径回放该命令行，回放前以 `-> <line>` 黄/蓝回显实际执行行；任意其他主循环行覆盖并清除 pending；回放一次性消费后立即清除。
- **隔离**：pending 识别只挂主提示符循环；PE->CE offer、format 闸门、gfx consent、useradd wizard 等内部 y/n 问询走各自 `kb_readline`/`kb_getkey`，不持有 pending 状态，其回答不受影响；autostart 脚本模式（g_script_mode）不 arm。
- **测试**：新增 `tests/shell_guess_regress.py`（两趟：format 后注入 mkpe 样本 PE），16 项断言覆盖 y 直跑、大写 Y、参数携带且不残留、无 pending 时 y 报 unknown、PE 问询 y 不误触发、连续两次错误 pending 覆盖；已接入 `tests/run_full_regress.sh`（端口 4450）。
- **回归**：本轮实测 qemu 12/12、guess 16/16；改动为 shell 主循环附加状态机，未触碰解析/补全/路径查找，其余套件按 v2.46 基线（fsrec 12/12、caps 12/12、user 15/15、disk 5/5、env 13/13、heap 4/4、size 7/7；ce 11 FAIL 为 ce_sim 未建模既有基线）。

## 二十七、v2.48 LFN 写侧健壮性（fat.c）

- **非法字符过滤**：新增 `fat_name_illegal()`（fat.c/fat.h），拒绝控制码与 FAT 保留标点 `" * / : < > ? \ |`，以及尾随空格/点；`fs.c` `create_path()` 用户态入口与 `fat_create_nl()` 内核侧双重校验，非法名返回 `FS_ERR_INVAL`（shell 报 `invalid name`）。
- **别名碰撞核对**：`make_alias83()` 按 Windows 短名规则从词干取前 6 字符时剔除点号/空格，非法可打印字符替换为 `_`；`~1..~99` 递增前逐个 `fat_lookup()` 核对 8.3 命名空间（既有 LFN 条目的别名即其短条目，8.3 扫描同时覆盖两种布局）；99 个别名耗尽返回失败。
- **失败路径回滚安全**：`dir_put_run()` 记录每个已写簇与槽位区间；中途 ATA 失败或目录链越界时，把已写槽位逐个墓碑化为 `0xE5` 并重写回盘，不留悬挂的半截 LFN run / 半写短条目。`dir_write_lfn()` 链尾追加新目录簇失败时，解除父簇链接并释放新簇，不再留下孤儿链项；数据簇分配失败仍由调用方 `fat_free_chain()` 回收。
- **lcase 字节（ntres, offset 12）**：新增 `ntres_from_name()`，无损 8.3 条目按原始大小写写 `0x10`（词干全小写）/`0x08`（扩展名全小写）；LFN 别名恒为大写、ntres=0。`dent_init()` 增加 ntres 参数，`fat_mkdir/fat_mkfile` 保留原签名并委托 `_nt` 变体。
- **未做**：原生 rename（copy+delete 现状保留，批次 C）；LFN 上限 240 码元与规范 255 的差距未动；读侧展示仍统一小写（8.3 路径），LFN 路径保留原始大小写。
- **测试**：新增 `tests/lfn_regress.py`（14 项：长名建/列/删往返、7 类保留字符拒绝、同词干别名碰撞共存、多点名往返、混合大小写 8.3 与 LFN 两路径、无致命 fault），接入 `run_full_regress.sh`（端口 4712）。
- **回归**：本轮实测 lfn 14/14、qemu 12/12、fsrec 12/12；纯 fs 层改动，未触 CE ABI 与 shell 主循环。

## 二十八、v2.49 libc.a 雏形——公共 CE 应用层（ce_app.c/ce_app.h）

- **现状收敛**：此前 32 个 CE 应用各自重复同一段入口样板（`__ce_api` 槽定义 + `#include ce_abi.h`），字符串/控制台辅助有的走 vsyscall 槽、有的 app 内手写。本轮抽出公共层 `extapp/ce_app.c` + `ce_app.h`：槽定义只留一份（weak 符号，未改造的旧 app 强定义覆盖之），头文件提供 `ce_write/ce_puts/ce_putc/ce_cls/ce_strlen/ce_strcmp/ce_strcpy/ce_atoi` 内联包装——优先走内核字符串槽，槽缺失时回落纯用户态实现。
- **构建方式（取改动小的方案）**：暂不做 ar 归档与独立 crt0；`build_ce.sh` 在既有 `ce_stub.o` 旁多编一个 `ce_app.o` 链接进每个镜像。归档库（libc.a）与启动代码分离列为后续小设计。
- **首批改造 4 个典型应用**：`hello.c`、`spinner.c`、`sysinfo.c` 移除本地槽定义、改引 `ce_app.h`；`demo.c` 除槽收敛外，4 处字符串调用（strcpy/strlen/strcmp/atoi）切换到公共包装。其余 app 暂保持原样，weak 桥接保证链接不破；cc/cc_self/ocntools 等有自定义类型或双定义的特例本轮不动。
- **测试**：全量重建 42 个 .ce 无重复定义/链接错误；真机探针 hello/spinner/sysinfo 三应用均正常出输出、无 fatal fault。
- **回归**：本轮实测 qemu 12/12、fsrec 12/12（内含改造后 hello.ce 内容完整性校验）；纯用户态构建侧，未触内核。

## 二十九、v2.50 递归操作语义补齐（shell.c / fs.c）

- **覆盖/合并语义**：`cmd_cp` 新增 `cp_target()`——DST 为已存在目录时归一化为 `DST/<src 基名>`（POSIX 合并语义），文件/目录两分支都走归一化目标；否则原样作为覆盖/改名目标。
- **权限位保留**：`cp_src_mode()` 查源 ACL 模式位，`copy_rec` 目录分支与 `fs_copy_file` 文件分支都把源模式位带到目标（owner 仍为创建者；`cp -p` owner 保留未实现）；无 ACL 条目回落 0755。
- **rmdir 命令**：新增 `cmd_rmdir`，拒绝 `/` 与非目录；`fs_rmdir` 返回码映射 -3 非空 / -2 非目录或根 / -1 不存在；命令派发、help、两处补全表已接线。
- **fs_rmdir 拆分 strict/force**：`fs_rmdir`（strict）供 `rmdir` 命令做非空拒绝；`fs_rmdir_force` 供 shell 既有 `rm -r` 后序清空器复用，行为与 v2.24 基线一致。
- **未做 / deferred**：mtime 时间戳保留——fs/fat 层不暴露 mtime 读/写 API，`fs_entry_t` 无日期字段，ls 不显示时间；需新增 stat + set-time 接口（设计级，后续批次）。深度上限 `FSREC_DEPTH_MAX=6` 保留未放宽（放宽需先评估栈风险：每帧两个 FS_PATH_MAX 缓冲）。
- **测试**：新增 `tests/recur_regress.py`（10 项：cp -r 合并嵌套、文件入目录、000 模式保留、对照保留执行位、rmdir 空目录删除 / 非空拒绝 / 文件拒绝、无 fatal fault），接入 `run_full_regress.sh`（端口 4713）。
- **回归**：本轮实测 recur 10/10、fsrec 12/12；qemu 12/12 在 force 拆分前一轮实测通过（拆分后仅 rm_rec 切到 force 变体，未再单跑 qemu）。
