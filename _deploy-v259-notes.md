# CatOS v2.59 本地部署说明（2026-11-19）

## 结果
- 云主机仓库 HEAD = `46e5f47`（`docs: push-log 补 5fd43d0`，v2.59 收口版本）。
- 本地 `D:\CatOs` 工作树已同步到该提交：`git status` 的 tracked 差异为 **0**。
- 本地分支：`main` → `46e5f47`（工作分支）；`master` 与 `backup-local-db64d8b` 均停在旧提交 `db64d8b`（v2.26 时期），保留可回退。
- 远端：`vm`（来自 bundle）指向同一历史；原 `origin` 仍为 `ocean-2026/catos-os.git`，未改动。

## 关键文件（已就位）
| 路径 | 说明 |
|---|---|
| `kernel_v25/src/pager.c` (5685B) | 通用分页器 |
| `kernel_v25/src/ime.c` (7370B) / `pinyin.inc` | 中文输入法阶段一 |
| `kernel_v25/src/bsod.c` (24842B) / `serial_log.c` / `symtab.inc` | panic / CPU 异常界面 |
| `kernel_v25/src/keys_table.c` / `docs/快捷键.md` | 快捷键命令与清单 |
| `docs/final-report-20261119.md` (17663B) | 最终报告（含 §8 双审结论） |
| `docs/CatOS-内核问题清单.md` | 已用至 #212 |
| `tests/run_full_regress.sh` + 22 套件 | 回归入口（22 套件 / 270 断言 / 269 PASS） |
| `catos-v2.59.iso` (11,649,024B, md5 `3bdd7fed243a6a8608c0c2472a872c2f`) | 可直接启动的镜像；2026-11-19 14:15 在干净工作树（HEAD `46e5f47`）重建，连续两次重建字节完全一致，自检 pager 27/27、ime 19/19、panic 19/19、mouse 18/18 |

## 备份与留痕
- `git stash`：`stash@{0}` = `pre-v259-local-edits`（部署前本地 308 项改动全量存入，可 `git stash show -p` 查看、`git stash pop` 回放）。
- `D:\CatOs\_backup_untracked_v259\`：部署前 287 个"本地未跟踪但新树已跟踪"的文件（含 `docs/night-P0-shots/*`、`docs/night-P0-report.md`、`tests/qemu_shots_p4/*` 等）原样保留。
- `D:\CatOs\_incoming_v259\`：本次传输件（源码 zip 6.3MB / git bundle 7.5MB / iso），md5 与云主机一致。

## 已知差异（仅 1 处，无法规避）
- 仓库内存在名为 `nul` 的受版本控制文件，Windows 保留设备名不允许创建，故本地缺失；已用 `git update-index --skip-worktree nul` 屏蔽，`git status` 保持干净。该文件无实际用途，建议后续在 Linux 侧提交一次删除。

## 本地启动方式（QEMU 示例）
```powershell
qemu-system-i386 -m 256 -cdrom D:\CatOs\catos-v2.59.iso -boot order=d
```
启动后默认进图形模式 gfx24（800×600×24）；`/etc/videomode` 可改，VBE 缺失时自动回退文本模式。
鼠标 / IME / 分页器 / 崩溃屏的验收截图见云主机 `shots-delivery-20261119/`（14 张）。

## 下一步（未执行，等你指令）
1. 在 Windows 本地跑一次 QEMU 冒烟（需要本地 QEMU 或 WSL）。
2. 把 `nul` 从仓库删除并在两侧同步。
3. 清理 `_backup_untracked_v259` / `_incoming_v259` 与历史磁盘镜像（当前目录 11.3GB，D 盘余量 37.5GB）。
