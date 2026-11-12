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
