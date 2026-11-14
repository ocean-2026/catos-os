# CatOS P0 Spawn 执行权限检查（2026-11-11）

> 对应 ROADMAP P1 / 问题清单 #96：`task_spawn_ce` 补上 `PERM_EXEC` 检查。

## 背景

ROADMAP P1 第一项待办：`spawn 执行权限检查`。v2.13 已引入多用户 + ACL，但 `task_spawn_ce()` 只校验路径存在与镜像大小，**从不查 `PERM_EXEC`**。这意味着 uid=100 可以 `run /root/malicious.ce`——写保护只防"改文件"而不防"跑文件"。

## 改动

### kernel_v25/src/task.c (`task_spawn_ce` 入口之后)

在 `fs_resolve_file` 成功、`size >= 4` 之后加入两段检查：

1. **快速拒绝无 x 的文件**：`acl_lookup(path, &fuid, &fgid, &fmode)` 找到 ACL 项且 mode 不含 `0o111` → 直接返 -2（不引入新错误码，保持错误语义一致）。
2. **按 owner/group/other 语义重验**：取调用者 uid 对应的 gid，调已有 `fs_perm_check(path, uid, gid, PERM_EXEC)`；root 直通，其他按 12 位 Unix 模式算。

```c
    /* v2.15: enforce PERM_EXEC on the path before loading. */
    {
        int fuid, fgid, fmode = 0;
        if (acl_lookup(path, &fuid, &fgid, &fmode) == 0 &&
            !(fmode & 0o111)) {
            return -2;   /* no execute bit at all */
        }
        user_rec_t *u = user_by_uid(uid);
        if (fs_perm_check(path, uid, u ? u->gid : 0, PERM_EXEC) != 0)
            return -13;  /* EACCES */
    }
```

### 文档更新

- `docs/CatOS-内核问题清单.md` §十八：新增 #96 条目 + 代码 diff + 行为影响评估。

## 验证状态

| 测试 | 结果 |
|---|---|
| `python3 tests/test_app_list.py` | ✅ exit=0（apps.list 一致性完好） |
| `python3 tests/test_mkce.py` | ✅ exit=0（CE 构建器未受影响） |
| `sh kernel_v25/build.sh iso` | ✅ ISO 重新生成 |
| `python3 tests/ce_sim.py bin/hello.ce` | ✅ 模拟器通过 |
| QEMU 实机回归（qemu/user/ot/diag/disk/vprobe/snd/go/gfx24/heap） | 🔶 需云侧环境复测 |

## 已知影响面

- `/bin/*.ce`（mode=0755）：全部 OK。
- `/home/user/*.ce`（mode=0644）：**需要用户先 chmod +x**，这是设计意图（安全增强）。
- 错误码：无 x → -2（ENOTFOUND-ish），无权限 → -13（EACCES），两种都是调用方已有的 "cannot load err N" 处理。

## 下一步（可选增强）

- 加 `tests/user_regress.py` 步骤验证 guest 下 `chmod 000` 的文件不能被 spawn。
- QEMU 实机跑全套回归确认 12/12 / 15/15 / 16/16 全绿。
