# F11 口令哈希不可导出 — 验收报告

- 工作树：`/home/user/work/catos-f11`
- 分支：`day-f11`（基线 commit `cf0ee35`）
- 范围：口令摘要加固、`/etc/passwd` 访问控制、非 root 可见性掩码、常量时间比较
- 结论：**已完成，三场景 + 冒烟均在真实 QEMU 中实测通过**；未验证项见第 10 节，未写成通过。

## 1. 改动文件清单

| 文件 | 改动 |
|---|---|
| `kernel_v25/include/user.h` | `pw[64]` 扩为 `pw[96]`；补充 v3 `$C$` 格式注释；导出 `user_db_dirty()`。 |
| `kernel_v25/src/user.c` | 引入 `crypto.h`；新增 hex 编解码与常量时间比较 `pw_ct_eq()`；`user_pw_set_rec()` 生成 v3 记录；`user_check_pw()` 新增 `$C$` 分支，v2/v1/djb2 校验成功后原地升级 v3 并置 `users_dirty`；比较全部改为异或累加、不提前 return；`user_save()` 与 `user_db_init()` 强制 `/etc/passwd` ACL 为 root:root 0600 并持久化；`acl_init()` 重构出 `acl_seed_defaults()`，已存在 `/etc/acl` 时回填缺失的 scratch/system 默认条目。 |
| `kernel_v25/src/shell.c` | `cmd_lsuser()`：非 root 口令列掩码为 `*`，root 仅显示方案标记（`$C$`/`$S$`/`hex`），不显示完整摘要；`cmd_login()`：认证成功后若表有未持久化改动（老记录升级）立即 `user_save()` 落盘。 |
| `kernel_v25/src/posix.c` | 命令注册表放宽 `useradd` maxpos 1→4、`login` maxpos 1→2（minpos 不变，裸命令仍走交互向导），与内建命令实际参数形态一致。 |
| `docs/全局待办总表.md` | 仅更新 F11 行状态与完成摘要，未重写表结构。 |

未改动：`tls.c`、`tls_aes.c`、仓库内 `catos_disk.img`；主树 `/home/user/work/catos-os` 未触碰；未 push、未 merge main。

## 2. v3 口令摘要格式定义

### 2.1 记录串

```
$C$<16 hex salt>$<64 hex digest>
```

- 前缀：字面量 `$C$`。
- 盐：8 字节（64 位）随机盐，编码为 16 个小写 hex；由现有 `pw_salt_gen()` 生成（IRQ tick + RTC + 指针地址雪崩），**每账户、每次设置口令都不同**；两个同口令账户的摘要必须不同（实测见 8.2）。
- 摘要：`digest = SHA-256(salt_bytes[8] || password_bytes)`，32 字节，编码为 64 个小写 hex。
- 复用内核已有 `sha256_2()`（`crypto.c`），未引入新的密码原语。
- 存储位置：`user_rec_t->pw`，对应 `/etc/passwd` 第 7 字段；记录总长 3+16+1+64 = 84 字符 + NUL，`pw[96]` 足够容纳。

### 2.2 实测样本（场景一，同口令 `AlicePass1`）

```
alice: $C$2d7462dac3bdb296$002f5c031a6bba4f8e413be1e5872769a54e2978fa3e36c179a4e4b07274ae68
bob:   $C$2d0dc4b8656ef1c2$8512a8c51759f54a84f863f645ee0dad8a9568b2b9e8493e217610c12acf3231
```

盐与摘要均互不相同，同口令不产生可关联记录。

## 3. 权限模型

### 3.1 文件 ACL

- `/etc/passwd` 的 ACL 固定为 **root:root 0600**（owner root，group root，仅 owner 读写）。
- 强制设置在两个入口：
  1. `user_db_init()`：从磁盘加载（或播种默认账户）之后，重新断言该 ACL 并 `acl_save()`，覆盖旧卷上缺失或更弱的条目。
  2. `user_save()`：先 `fs_delete` 再 `fs_write_raw` 写盘之后，`acl_set("/etc/passwd", UID_ROOT, 0, 0600)` + `acl_save()`。
- 说明：`acl_default()` 不覆盖已存在条目，因此这里用 `acl_set()` 强制写入；ACL 键是路径，删文件不会清 ACL，但两个入口都再次确保。
- `acl_seed_defaults()` 仅回填缺失的 `/home`、`/tmp`、`/bin`、`/appdevelop`、`/etc`、`/root` 默认条目，已持久化的 `/etc/acl` 记录优先，不影响 `useradd`/`passwd`/`chcaps` 的正常持久化。

### 3.2 读取闸门与可见性

- 权限判定走 `fs_perm_check()`：root（uid 0）直接放行；非 root 对 0600 的 `/etc/passwd` 读被拒。
- 实测读取路径：`edit -c /etc/passwd`（即 `cmd_cat` 路径，shell.c 命令分发）；CE 文件读 syscall 同样经过该检查。`cat` 已是退役别名，转发到 `edit -c`，本身不打开文件。
- 列表命令：`lsuser` 与 `users` 共用 `cmd_lsuser()`。非 root 会话口令列显示 `*`；root 会话列表只显示方案标记（`$C$`/`$S$`/`hex`），不打印完整摘要。
- 其他命令核对：
  - `whoami`：仅打印用户名。
  - `id`：仅打印 `pw=set` / `pw=none`，不含摘要材料。
  - `caps`：仅打印能力位图例。
  - 以上命令均不泄露口令摘要。

## 4. 迁移兼容策略

### 4.1 保留校验的老格式

| 格式 | 形态 | 说明 |
|---|---|---|
| djb2 裸 hex | 8 hex（无 `$` 前缀） | 最老记录。 |
| v1 | `$S$` + 8 hex 盐 + `$` + 8 hex 摘要 | 32 位 FNV 变体。 |
| v2 | `$S$` + 16 hex 盐 + `$` + 32 hex 摘要 | 128 位四 lane FNV。 |
| v3 | `$C$` + 16 hex 盐 + `$` + 64 hex 摘要 | 本次新增，加盐 SHA-256。 |

### 4.2 升级流程

1. `user_check_pw()` 按前缀识别格式，用对应老算法校验。
2. 校验**成功**：在内存中把该记录原地重写为 v3 `$C$`，置 `users_dirty = 1`。
3. `cmd_login()` 认证成功后检查 `user_db_dirty()`，若有升级立即 `user_save()` 落盘，不必等下一次 `useradd`/`passwd` 变更。
4. 校验失败：不升级，记录保持原样。

## 5. 常量时间比较

- 新增 `pw_ct_eq(a, b, n)`：循环内 `diff |= a[i] ^ b[i]`，全程不提前 return，避免逐字节时序泄露。
- v3 摘要比较使用该函数；v2 四 lane、v1、djb2 的比较也改为异或累加后再判等，不再在首个不匹配字节处返回。

## 6. 验证环境与构建

- 真机：`qemu-system-i386`（6.2.0），`-m 256 -no-reboot -net none -display none`，ISO 为 `kernel_v25/catos.iso`，磁盘挂载 `test_disk.img` 的私有副本（`/tmp/catos_f11/f11_*.img`），每个场景先复制基准盘再挂载，不污染基准副本；monitor 使用独立端口（4480 段，避开 4444 与其他会话占用口）。
- 构建（worktree 根目录）：

```sh
sh build.sh kernel
sh build.sh iso
```

未执行 `disk` 目标，未修改仓库内 `catos_disk.img`。
- 取证：HMP monitor 读屏文本转储为 `*.txt`；`screendump` PPM 经 `convert` 转 PNG 存 `shots/`。

## 7. 实测结果（真实输出）

### 7.1 冒烟：ISO 启动到 shell，无 panic

输出（`smoke_lsuser.txt` / `shots/smoke_boot.png`）：

```
root@catos:/# whoami
root
root@catos:/# lsuser
  UID  GID  PW   CAPS      NAME
  0000 0000 no   FFFFFFFF  root
  0100 0100 no   0000002B  user
```

启动到 shell 提示符正常，无 panic；种子账户 root(0)/user(100) 在位。

### 7.2 场景一：格式与权限

操作：root 下 `useradd alice AlicePass1`、`useradd bob AlicePass1`（同口令）；`login alice AlicePass1`。

root 视角 `lsuser`（`s1_nonroot_denied.txt` 上半段）：

```
root@catos:/# lsuser
  UID  GID  PW   CAPS      NAME
  0000 0000 no   FFFFFFFF  root
  0100 0100 no   0000002B  user
  0101 0101 $C$  00000000  alice
  0102 0102 $C$  00000000  bob
root@catos:/# login alice AlicePass1
  logged in as alice (uid 101)
```

非 root（alice）读 `/etc/passwd` 被拒（`s1_nonroot_denied.txt` / `shots/s1_nonroot_denied.png`）：

```
alice@catos:/$ edit -c /etc/passwd
  cat: permission denied
```

非 root `lsuser` 口令列掩码（`shots/s1_nonroot_lsuser_mask.png`）：

```
alice@catos:/$ lsuser
  UID  GID  PW   CAPS      NAME
  0000 0000 no   FFFFFFFF  root
  0100 0100 no   0000002B  user
  0101 0101 *    00000000  alice
  0102 0102 *    00000000  bob
```

root 重新登录后 `edit -c /etc/passwd` 可见两条 `$C$` 记录，摘要不同（`s1_root_passwd_records.txt` / `shots/s1_root_passwd_records.png`）：

```
root@catos:/# edit -c /etc/passwd
  root:0:0:/root:/bin/shell.ce:ffffffff:
  user:100:100:/home/user:/bin/shell.ce:2b:
  alice:101:101:/home/alice:/bin/shell.ce:0:$C$2d7462dac3bdb296$002f5c031a6bba4f8e413be1e5872769a54e2978fa3e36c179a4e4b07274ae68
  bob:102:102:/home/bob:/bin/shell.ce:0:$C$2d0dc4b8656ef1c2$8512a8c51759f54a84f863f645ee0dad8a9568b2b9e8493e217610c12acf3231
```

### 7.3 场景二：持久化与迁移

#### 7.3.1 重启后账户可用

复制场景一磁盘副本重启，alice 用原口令 `AlicePass1` 登录成功（`s2_persist_login.txt` / `shots/s2_persist_login.png`）：

```
root@catos:/# login alice AlicePass1
  logged in as alice (uid 101)
alice@catos:/$ whoami
  alice
```

证明 0600 ACL 与 v3 新格式在重启后均持久，且不影响读库登录。

#### 7.3.2 老格式记录登录即升级

手工构造含三类老记录的盘（`s2_migration_before.txt`，主机侧按老算法转录摘要）：

```
carol:103:103:/home/carol:/bin/shell.ce:0:$S$1122334455667788$2f299a2a44c03fa55c09353dcc0801f4
dave:104:104:/home/dave:/bin/shell.ce:0:$S$0a0b0c0d$4d83b993
erin:105:105:/home/erin:/bin/shell.ce:0:2610fdbb
```

- carol（v2）：`CarolPass1` 登录成功；
- dave（v1）：`DavePass1` 登录成功；
- erin（djb2）：`ErinPass1` 登录成功。

回查盘上记录（`s2_migrated_after.txt` / `shots/s2_migrated_after.png`），三条全部升级为 `$C$`，无 `$S$`/裸 hex 残留：

```
carol:103:103:/home/carol:/bin/shell.ce:0:$C$3e7e0fe910916026$37f8be1544e9d93422e7c7f2fe2d698f472513f061b25bc7b8040496c4b91490
dave:104:104:/home/dave:/bin/shell.ce:0:$C$09541b842914cd4d$38f7c1d418220ce59ecadfb8b29b710ac7c0a576d025eb53c9b85f24bee87a31
erin:105:105:/home/erin:/bin/shell.ce:0:$C$6b7139d896b20de5$77553597940c6f199b8e1788bdce28aa89e151190782f4b2e8c96a5b837263b4
```

### 7.4 场景三：回归

#### 7.4.1 口令轮换：旧口令失效、新口令可登录

`s3_old_password_rejected.txt` / `shots/s3_old_password_rejected.png`：

```
root@catos:/# useradd reguser 106 106 0
  user added (persisted to /etc/passwd)
root@catos:/# passwd reguser OldPass1
  password set
root@catos:/# login reguser OldPass1
  logged in as reguser (uid 106)
reguser@catos:/$ login root
  logged in as root (uid 0)
root@catos:/# passwd reguser NewPass2
  password set
root@catos:/# login reguser OldPass1
  login: incorrect password
```

`s3_new_password_accepted.txt` / `shots/s3_new_password_accepted.png`：

```
root@catos:/# login reguser NewPass2
  logged in as reguser (uid 106)
```

#### 7.4.2 二次确认访问控制

非 root（tmpuser）读 `/etc/passwd` 被拒、`lsuser` 掩码（`shots/s3b_nonroot_denied.png`）：

```
tmpuser@catos:/$ edit -c /etc/passwd
  cat: permission denied
```

root 重新登录后可读，文件含 `$C$` 记录（`shots/s3b_root_readable.png`）：

```
root@catos:/# edit -c /etc/passwd
  root:0:0:/root:/bin/shell.ce:ffffffff:
  user:100:100:/home/user:/bin/shell.ce:2b:
  tmpuser:108:108:/home/tmpuser:/bin/shell.ce:0:$C$dd7fab3bc8f64383$9bc6f9cf0e8b...
```

#### 7.4.3 `userdel` 删除回归

`s3c_userdel_gone.txt` / `shots/s3c_userdel_gone.png`：

```
root@catos:/# useradd deluser 109 109 0
  user added (persisted to /etc/passwd)
root@catos:/# passwd deluser DelPass1
  password set
root@catos:/# userdel deluser
  user deleted
root@catos:/# lsuser
  UID  GID  PW   CAPS      NAME
  0000 0000 no   FFFFFFFF  root
  0100 0100 no   0000002B  user
```

#### 7.4.4 `cat` 路径回归

非 root 的 `cat /etc/passwd`（退役别名，转发 `edit -c`）同样被读闸门拒绝；root 的 `edit -c` 可读。

## 8. 证据清单

目录前缀：`/home/user/work/catos-f11/artifacts/f11-passwd/`

### 8.1 截图 PNG（`shots/`）

| 文件 | 对应验收点 |
|---|---|
| `shots/smoke_boot.png` | 冒烟：启动到 shell，无 panic。 |
| `shots/s1_nonroot_denied.png` | 非 root 读 `/etc/passwd` 被拒。 |
| `shots/s1_nonroot_lsuser_mask.png` | 非 root `lsuser` 口令列掩码 `*`。 |
| `shots/s1_root_passwd_records.png` | root 可见 alice/bob 两条 `$C$`，摘要不同。 |
| `shots/s2_persist_login.png` | 重启后 alice 原口令登录成功。 |
| `shots/s2_migrated_after.png` | 迁移后 carol/dave/erin 全部 `$C$`。 |
| `shots/s3_old_password_rejected.png` | 改密后旧口令被拒。 |
| `shots/s3_new_password_accepted.png` | 新口令登录成功。 |
| `shots/s3b_nonroot_denied.png` | 非 root 二次确认被拒。 |
| `shots/s3b_root_readable.png` | root 可读 `/etc/passwd` 含 `$C$` 记录。 |
| `shots/s3c_userdel_gone.png` | `userdel` 后账户从列表消失。 |

另有调试帧（非交付必需）：`shots/debug2_*.png`、`shots/debug_final.png`、`shots/s3_nonroot_denied.png`。同名 `.ppm` 为 screendump 原始帧。

### 8.2 文本转储（`*.txt`）

`smoke_lsuser.txt`、`s1_nonroot_denied.txt`、`s1_nonroot_lsuser_mask.txt`、`s1_root_passwd_records.txt`、`s2_persist_login.txt`、`s2_migration_before.txt`、`s2_migrated_after.txt`、`s3_old_password_rejected.txt`、`s3_new_password_accepted.txt`、`s3b_nonroot_denied.txt`、`s3b_root_readable.txt`、`s3c_userdel_gone.txt`、`s3_nonroot_denied.txt`、`s3_root_readable.txt`。

其中 `s1_nonroot_lsuser_mask.txt`、`s3b_nonroot_denied.txt`、`s3b_root_readable.txt` 因 monitor 文本读取间歇为空（见 10.3），对应结论以同名 PNG 截图为准。

### 8.3 探针脚本（`*.py`）

- `f11_lib.py`：启动/等待提示符/命令注入/截图/抓屏公共封装。
- `smoke.py`：冒烟启动。
- `s1_format_perms.py`、`s1_root_records.py`：场景一。
- `s2_persist_migrate.py`：场景二持久化 + 迁移（含 FAT 补丁与老记录构造）。
- `legacy_hash.py`：老格式（djb2/v1/v2）摘要主机侧转录。
- `s3a_rotation.py`、`s3b_access_userdel.py`、`s3c_userdel.py`、`s3_regression.py`：场景三回归。
- `debug_login.py`、`debug_reboot_root.py`、`debug_s3.py`、`diag_monitor.py`：调试用，非验收探针。

## 9. 文档更新

- `docs/全局待办总表.md`：F11 行已标记 ✅，写入完成摘要与报告指引；仅改该行，表结构未动。
- `docs/SECURITY-v2.67.md`：仅有 USERADM 能力位表提及 `useradd`/`passwd`，无 `/etc/passwd` 文件或口令摘要专条，按口径未追加。
- `docs/CatOS-内核问题清单.md`：用户模型段与 #185/#186 已修条目中无 F11 直条目，未追加。

## 10. 未做 / 做不通 / 限制项（如实记录）

1. **ssh / 网络功能未测**：任务范围外，本次只验证本地 shell 与文件访问路径。
2. **未引入内存硬化 KDF**：本次仅换用内核已有的 SHA-256 + 每用户盐；Argon2/scrypt 等内存硬化方案仍在路线图，当前对离线字典扫描的抵抗有限。
3. **`cat` 为退役别名**：读取闸门验证在 `edit -c`/`cmd_cat` 路径；`cat` 命令本身转发到该路径，不直接打开文件。
4. **测试环境抖动**：长会话后 monitor `xp` 读屏间歇返回空白，偶发 "ce_main not at image offset 0"、提示符延迟出现；均为重跑即恢复的已知抖动，关键结论以 PPM→PNG 截图佐证，未仅凭空白文本下结论。
5. **控制台折行**：80 列文本模式下 64 hex 摘要会折到第二行显示，文件内容本身完整。
6. **未做的仓库约束遵守情况**：未 push、未 merge main；未改动主树 `/home/user/work/catos-os`；未改 `tls.c`/`tls_aes.c`；未改仓库内 `catos_disk.img`。
