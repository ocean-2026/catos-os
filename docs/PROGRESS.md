# CatOS 进度汇报（2026-11-11）

> 覆盖窗口：2026-11-07 合流之后至 2026-11-11 · 主线 `D:/CatOs` master
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

---

## 三、回归总账（#91 后同 ISO 全套）

```
实机: user_regress 15/15  qemu_regress 12/12  diag_regress 4/4  disk_regress 5/5
      vprobe_regress 10/10  snd_regress 6/6  gfx24_regress 0 failed
      heap_regress 4/4  go_regress 7/7  qa_probe_final exit=0
宿主: test_mkce 48  test_cc 10  test_app_list 21 apps  ce_regress 11P/13S/0F
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
