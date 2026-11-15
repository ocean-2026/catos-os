# CatOS 跨夜迭代 P2 分片报告：磁盘分区与 Linux 式挂载

- 工作副本：`/home/user/Doubao/chats/38453665424926722/night/CatOs-P2`（基线 commit `9e47280 v2.37`）
- 构建：`sh build.sh` → `kernel_v25/catos.iso`（构建成功，`OK -> build/kernel.bin`、`OK -> catos.iso`）
- 内核：32 位 x86，QEMU/i386，GRUB multiboot，ring0 shell
- 实机验证端口段：4720（本分片）；QEMU：`/home/user/bin/qemu-system-i386`（6.2.0）

## 1. 验收点对照

| 任务要求 | 实现情况 | 实机证据 |
|---|---|---|
| MBR 分区表解析：4 个主表项，识别 FAT32 | `part.c` 扫描每块盘 LBA0 的 4 项；识别类型 `0x0B/0x0C` 为 FAT32 | p2_03 / p2_12（`mount 2 /docs` 成功） |
| 扩展分区不支持时给出明确提示 | `0x05/0x0F/0x85/0xEE/0xC5` 标记 `extended`；`mount` 返回 “unsupported partition (extended or non-FAT32)” | 代码路径，未见扩展分区测试盘 |
| `mount <分区号/设备> <目录>` 挂到已存在空目录，Linux 语义 | `fs_mount()`：目标必须存在、是目录、为空、非挂载点；支持分区号 `1..4`（drive0）或 `hd<d>p<e>` | p2_03_mount |
| 裸 `mount` 列出挂载表 | `fs_mount_list()`：根卷 + 各挂载点一行 | p2_01_mount_table |
| 被挂载文件夹名后附加盘符（如 `docs(C:)`） | 列目录时按完整路径反查挂载表，追加 `(B:)`；本分片首挂盘符为 `B:`（根卷 `A:`） | p2_04_ls_suffix |
| `umount` 卸载并恢复原名 | `fs_umount()`；后缀仅展示时附加，卸载后目录恢复原名 | p2_09_umount / p2_10_after_umount / p2_11_ls_restored |
| `df` 按挂载点逐行列总/已用/可用 | `fs_df()` 逐挂载点激活对应卷统计；`hd0 /` 与 `hd0p2 /docs` 两行 | p2_08_df |
| 挂载点路径在 VFS 层打通，读写落到对应分区 | 路径解析最长前缀匹配命中挂载点后切到该卷继续解析；跨分区读 `note.txt`、跨分区写 `hello.ce` | p2_05 / p2_06 / p2_07 |
| 跨分区读写一个文件 | 读：`cat /docs/note.txt` → `hello from partition 2`；写：`cp /bin/hello.ce /docs/hello.ce`（312 字节）；重启式重挂后文件仍在 | p2_06 / p2_07 / p2_12 |

## 2. 实现设计

### 2.1 块设备层（`ata.c` / `ata.h`）
- 启动时探测 4 个 IDE 槽位（主/从总线 × 主/从设备），每槽记录 `base/ctrl/dev/sectors/present`。
- 新增按盘读写：`ata_read(drive,...)` / `ata_write(drive,...)` / `ata_drive_sectors(d)` / `ata_drive_count()`。
- 保留旧单盘包装 `ata_read_sectors` / `ata_write_sectors` / `ata_identify`（走 drive 0），`hw.c` 等旧调用点无需改动。

### 2.2 分区表（新增 `part.c` / `part.h`）
- `part_scan()`：遍历已识别硬盘，校验 `55AA`，解析 4 个主表项（类型、活动位、起始 LBA、扇区数）。
- FAT32：`0x0B/0x0C`；扩展：`0x05/0x0F/0x85/0xEE/0xC5` 标记不支持。
- `part_lookup(spec)`：接受裸分区号（drive0，1 基）或 `hd<d>p<e>`；`part_devname()` 生成 `hd0p2` 这类设备名。

### 2.3 FAT 多卷上下文（`fat.c` / `fat.h`）
- 几何参数从单一全局变量改为按卷槽位保存：`g_vol[FAT_VOL_MAX=4]`（drive、分区起点、FAT 大小、数据区起点、根簇、分配提示、簇数、FAT 容量、总扇区、每簇扇区）。
- FAT 缓存与写日志仍为共享静态区，归“当前活跃卷”所有；`fat_activate(slot)` 切卷并从磁盘重载该卷 FAT #1、复位日志。这样避免每卷再占一份 4 MiB 缓存撑大 BSS。
- 抽出 `probe_volume(slot, drive, part_lba)`；新增 `fat_mount_partition(...)`（找空闲槽位探测）、`fat_activate()`、`fat_active_slot()`、`fat_release_slot(slot)`（卸载后归还槽位以便重挂）。
- 所有簇读写/镜像校验/FSInfo 都改走活跃卷所在盘（`cur_drive`），不再写死 drive 0。

### 2.4 VFS 挂载表（`fs.c` / `fs.h`）
- `g_mnt[3]`：挂载点绝对路径、卷槽位、盘符、设备名；根卷为隐式槽位 0（盘符 `A:`）。
- `mount_for(path)`：最长前缀匹配，判定路径归属哪个挂载点。
- `resolve_path()`：命中挂载点先 `fat_activate()` 切到对应卷，再在该卷内解析剩余相对路径；未命中走根卷。fs 层其余打开/读/写/列目录接口都经 `resolve_path`，天然跨卷。
- 挂载校验：目标存在、是目录、非已有挂载点、目录为空；卷探测失败返回细分错误码。
- 卸载保护：当前工作目录位于挂载点内则拒绝（busy）；卸载归还卷槽位。
- 展示：`list_cb` 对目录项按 `父路径/名字` 反查挂载表，命中则追加 `(X:)`，普通目录仍带 `/`；长格式同理。
- `fs_df()`：逐挂载点激活卷，输出 total/used/free/use%/挂载点。

### 2.5 shell 命令表（`shell.c`）—— **合并冲突预警**
- 新增 `cmd_mount` / `cmd_umount`；`cmd_df` 改为直接调用 `fs_df()`。
- 调度分支：`mount` / `umount` 接入命令分发；补全表 `sh_cmds` 增加 `mount`、`umount`；`help` 文件系统段补 4 行帮助。
- **冲突预警点**：命令分发 else-if 链、`sh_cmds` 补全数组、`cmd_help` 帮助表这三处其他分片很可能也在改，合并时需手动对齐。

### 2.6 构建脚本（`build.sh`）
- 编译源列表与链接对象列表加入 `src/part.c` / `build/part.o`。

## 3. 测试命令与结果

工作目录：`/home/user/Doubao/chats/38453665424926722/night/CatOs-P2`

### 3.1 本分片新增：挂载全流程回归
```
python3 tests/p2_mount_regress.py --qemu /home/user/bin/qemu-system-i386
```
结果：**16 checks, 0 failures**（含一轮 umount 后重挂，验证槽位复用与已写文件持久化）。

| 检查项 | 结果 |
|---|---|
| mount 列出根卷 | PASS |
| 挂载前 docs 目录存在 | PASS |
| mount 成功 | PASS |
| 挂载点显示 `(X:)` 后缀 | PASS |
| 挂载卷内 note.txt 可见 | PASS |
| 跨分区读内容正确 | PASS |
| 跨分区写文件落到挂载卷 | PASS |
| df 列出挂载点 /docs | PASS |
| df 列出根 / | PASS |
| umount 卸载 | PASS |
| 卸载后 note.txt 消失 | PASS |
| 后缀恢复 | PASS |
| 挂载表仅剩根卷 | PASS |
| 卸载后重挂成功 | PASS |
| 重挂再次看到分区内容 | PASS |
| 第二次 umount 成功 | PASS |

### 3.2 既有回归（全部在本副本跑过）
```
python3 tests/qemu_regress.py --qemu /home/user/bin/qemu-system-i386 --port 4722
# steps=12 ok=11 warn=1 （唯一 WARN 为 boot-shell 亮度 0.01，基线既有，非本次引入）

python3 tests/disk_regress.py --qemu /home/user/bin/qemu-system-i386 --port 4724
# steps=5 ok=5 fail=0

python3 tests/fsrec_regress.py kernel_v25/catos.iso --qemu /home/user/bin/qemu-system-i386 --port 4725
# fsrec_regress: 12/12 PASS
```

### 3.3 测试盘构造
- `tests/mkpart_disk.py`：宿主 Python 直接生成 32 MiB raw 盘，MBR 两个 `0x0C` FAT32 主分区；P1@LBA2048（含空 `docs` 挂载点目录），P2@LBA30720（含 `note.txt`）。复用 `mkdisk.py` 的 8.3 名/目录项打点逻辑，未依赖 sfdisk/mtools。
- 产物：`tests/qemu_disk_part.img`（`*.img` 已被 .gitignore 忽略，可由脚本重建）。

## 4. 截图清单（均已亲自 Read 核对画面内容）

目录：`/home/user/Doubao/chats/38453665424926722/night/CatOs-P2/tests/qemu_shots/`

1. `p2_01_mount_table.png` — 裸 `mount`：`hd0  /`
2. `p2_02_ls_root.png` — 挂载前根目录，`docs/` 普通目录
3. `p2_03_mount.png` — `mount 2 /docs` → `mount: /docs mounted`
4. `p2_04_ls_suffix.png` — `ls /`：`docs(B:)` 盘符后缀
5. `p2_05_ls_mounted.png` — `ls /docs`：P2 的 `note.txt (23 bytes)`
6. `p2_06_cat_note.png` — `cat /docs/note.txt` → `hello from partition 2`
7. `p2_07_write_copy.png` — `cp /bin/hello.ce /docs/hello.ce` 后 `ls /docs`：`hello.ce (312 bytes)`
8. `p2_08_df.png` — `df`：`hd0 14292K 460K 13832K 3% /`；`hd0p2 16336K 12K 16324K 0% /docs`
9. `p2_09_umount.png` — `umount /docs` → `umount: detached`
10. `p2_10_after_umount.png` — 卸载后 `ls /docs` 仅剩 `.`/`..`
11. `p2_11_ls_restored.png` — 根目录 `docs/` 后缀恢复
12. `p2_12_remount.png` — 卸载后重挂：`mount 2 /docs` 成功，`note.txt` 与持久化的 `hello.ce` 都在

## 5. 改动文件清单（供合并冲突预警）

### 新增（未跟踪）
- `kernel_v25/include/part.h` — 分区表模块接口
- `kernel_v25/src/part.c` — MBR 解析 / 分区查询
- `tests/mkpart_disk.py` — 双分区 FAT32 测试盘生成器
- `tests/p2_mount_regress.py` — P2 挂载全流程回归 + 截图
- `tests/qemu_shots/p2_*.png` — 12 张证据截图

### 修改（已跟踪）
| 文件 | 改动性质 | 冲突风险 |
|---|---|---|
| `kernel_v25/build.sh` | 编译/链接列表加入 `part.c`/`part.o` | 低：其他分片若也加新源文件会撞同一列表 |
| `kernel_v25/include/ata.h` | 多盘 API 声明 | 低 |
| `kernel_v25/include/fat.h` | 多卷 API 声明、`FAT_VOL_MAX` | 中：fs/fat 头被多方包含 |
| `kernel_v25/include/fs.h` | `fs_mount/fs_umount/fs_mount_list/fs_df` 声明 | 中：VFS 层头，其他 fs 分片可能同改 |
| `kernel_v25/src/ata.c` | 4 槽位探测 + 按盘读写；旧包装保留 | 中：块设备层，磁盘相关分片可能同改 |
| `kernel_v25/src/fat.c` | 几何多卷化、活跃卷缓存、挂载/释放接口、按盘读写 | **高：fat 内部状态被重构，几何变量全部改为卷上下文** |
| `kernel_v25/src/fs.c` | 挂载表、前缀路径解析、后缀展示、mount/umount/df | **高：VFS 层核心，路径解析与列目录回调被改动** |
| `kernel_v25/src/shell.c` | 命令分发、`sh_cmds` 补全表、help 表、`cmd_df` 改写、`cmd_mount/cmd_umount` | **高：命令表三处都在链上，最易与其他 shell 分片冲突** |
| `tests/qemu_driver.py` | 暂存目录改为按 PID 独立子目录（规避并行分片 QEMU 写锁冲突） | 低 |
| `tests/disk_regress.py` | 增加 `--port` 覆盖 | 低 |
| `tests/fsrec_regress.py` | 增加 `--port` 覆盖 | 低 |

> 重点提醒合并方：`fat.c` 的几何全局变量已迁入卷上下文结构；若其他分片基于旧全局符号打补丁，需要改走活跃卷访问器。`shell.c` 的命令分发 else-if 链、补全数组、help 表三处需逐行对齐。

## 6. 遗留问题 / 未覆盖项

1. **扩展分区（EBR 链）不遍历**：类型 `0x05/0x0F/0x85/0xEE` 仅标记不支持并由 `mount` 报错；没有解析 EBR 链式主分区。
2. **多物理盘仅代码路径验证**：ATA 扫描与分区扫描支持多盘，但金属测试只跑了“单盘双分区”；第二块 IDE 盘的挂载未在真机上验证。
3. **挂载表不持久**：`mount` 状态在内存，重启后需重新挂载；没有写回磁盘的挂载配置。
4. **卷上限**：含根卷共 `FAT_VOL_MAX=4`，即最多 3 个额外挂载卷；切卷时共享缓存从磁盘重载 FAT，卷很多时会有重复读 FAT 开销。
5. **根卷设备名硬编码**：挂载表/df 根卷固定显示 `hd0`；若根卷实际来自其他槽位，标签未动态化。
6. **挂载点权限模型**：复用既有 `fs_perm_check` 路径，没有为挂载目标单独建立属主/权限策略。
7. **不支持嵌套挂载**：往已有挂载点内再挂载会被拒绝（`mount_for` 命中即报错）；这是有意的简化，非 Linux 完整语义。
8. `qemu_regress` 的 `boot-shell` 亮度 WARN 为基线既有现象，本次未处理。

## 7. 复现步骤

```bash
cd /home/user/Doubao/chats/38453665424926722/night/CatOs-P2
sh build.sh                                   # 产出 kernel_v25/catos.iso
python3 tests/mkpart_disk.py -o tests/qemu_disk_part.img
python3 tests/p2_mount_regress.py --qemu /home/user/bin/qemu-system-i386
python3 tests/qemu_regress.py --qemu /home/user/bin/qemu-system-i386 --port 4722
python3 tests/disk_regress.py --qemu /home/user/bin/qemu-system-i386 --port 4724
python3 tests/fsrec_regress.py kernel_v25/catos.iso --qemu /home/user/bin/qemu-system-i386 --port 4725
```
