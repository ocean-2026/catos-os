# Stage5 自举环工作交接单

## 日期：2026-11-03

## 已完成工作

### 1. cc.c 同步
- 源文件：`extapp/cc.c`（97140 字节）
- MD5：`b73a0a29306ec537a7ffefd1be566580`
- 来源：`/c/Users/Useless/Downloads/catos-stage5-fix-2026-11-03.zip`
- 验证：与 stage5 zip 中 sources/cc.c 一致

### 2. 内核配置升级
- 文件：`include/paging.h`
- 变更：`USER_STACK_SZ` 从 `0x10000`（64K）升级到 `0x20000`（128K）
- 原因：CC2.CE 自举需要更大栈空间

### 3. 回归测试
- 测试程序：tests/HELLO.C, EXTRA.C, STRUCT.C, SWITCH.C, GOTO.C, CHARBUG.C
- 结果：6/6 PASS（使用现有 cc_self_host.exe）
- 注意：现有二进制是 Nov 1 构建，早于 forward-call-fix（Nov 2）

### 4. 参考值保存
- `build/HOSTCC2_REF.CE`：81522 字节，MD5 `eeec32f838fbab4c111d428df22aa409`
- 这是 Linux 机器上编译的预期输出

## 阻塞问题

### 问题 1：宿主编译器版本过旧
- 现状：`build/host/cc_self_host.exe`（167452 字节，Nov 1 构建）
- 问题：不含 forward-call-fix（L1829，Nov 2 应用）
- 症状：编译 cc.c 时报错 `type expected [token: punct *]`
- 解决：需要在 Linux 机器上重新编译

### 问题 2：Windows mingw32 工具链故障
- 现象：gcc 编译 .o 成功，但链接失败（exit code 49）
- 错误：`cannot find -lmingw32`, `cannot find -lmsvcrt` 等
- 已尝试：/c/msys64/mingw32, /d/msys/mingw32, /d/msys/ucrt64
- 原因：可能是 MSYS2 环境配置问题或库文件缺失

### 问题 3：QEMU 未安装
- 状态：仅有临时文件 `/c/Users/Useless/AppData/Local/Temp/qemu-setup.exe.part`
- 需要：完整安装 QEMU 进行回归测试

### 问题 4：内核 ISO 陈旧
- 当前：`catos.iso`（15921152 字节，Oct 31 构建）
- 需要：重新编译以包含 USER_STACK_SZ=0x20000 修改

## 下一步行动（下次会话）

### 步骤 1：在 Linux 机器上重建宿主编译器
```bash
cd /path/to/Catos_Better_better
gcc -m32 build/host_stub_cc.c extapp/cc.c -o build/host/cc_self_host.exe
```

### 步骤 2：验证自举编译
```bash
./build/host/cc_self_host.exe extapp/cc.c build/CC2.CE
md5sum build/CC2.CE
# 预期输出：eeec32f838fbab4c111d428df22aa409
```

### 步骤 3：重建内核 ISO
```bash
sh build.sh iso
ls -la catos.iso  # 应 ~10862592 字节
```

### 步骤 4：运行 QEMU 回归测试
```bash
python3 tools/cc_stage45_test.py
```

### 步骤 5：更新文档
- [ ] PROGRESS.md：标记 stage5 为完成
- [ ] history.md：添加 forward-call-fix 经验教训
- [ ] CatOS-内核问题清单.md：记录栈大小变更

## 关键文件位置

| 文件 | 路径 | 说明 |
|------|------|------|
| cc.c | `extapp/cc.c` | 自举编译器源（97140 bytes） |
| paging.h | `include/paging.h` | 内核配置（USER_STACK_SZ=0x20000） |
| cc_self_host.exe | `build/host/cc_self_host.exe` | 宿主编译器（需重建） |
| HOSTCC2_REF.CE | `build/HOSTCC2_REF.CE` | 参考目标（81522 bytes） |
| cc_stage45_test.py | `tools/cc_stage45_test.py` | QEMU 回归测试 |

## 技术细节

### Forward Call Fix（L1829）
```c
/* forward call fix (2026-11-02): a function prototype
 * (e.g. `static token_t *cur(void);`) created its own sym
 * with an invalid addr (no code emitted -> addr equals the
 * next function's).  A later call through that prototype
 * then resolved to the next function, producing a call to
 * itself (infinite recursion).  Reuse the prototype sym at
 * the definition so its addr gets the real code offset. */
sym_t *f = gfind(nm->sp, nm->len);
if (!f || f->kind != SY_FUNC) f = sym_new(nm->sp, nm->len, SY_FUNC);
```

### Bootstrap Fixed-Point 验证标准
- cc.ce 编译 cc.c → CC2.CE
- CC2.CE 编译 cc.c → CC3.CE
- CC2.CE 和 CC3.CE 字节级一致（md5 相同）
- 预期大小：81522 字节

## 备注
- Windows 环境尝试了多种编译方案均失败
- 建议直接使用 Linux 机器完成剩余工作
- 所有源文件已就绪，只需重建宿主编译器即可

## 2026-11-03 Windows 环境尝试记录

### 已尝试的编译方法

| 方法 | GCC 路径 | 结果 |
|------|----------|------|
| Bash gcc | /c/msys64/mingw32/bin/gcc.exe | exit code 49 (编译失败) |
| Bash gcc | /d/msys/mingw32/bin/gcc.exe | exit code 1 (链接失败) |
| Bash gcc | /d/msys/ucrt64/bin/gcc.exe | exit code 1 (库找不到) |
| PowerShell | C:\msys64\mingw32\bin\gcc.exe | exit code 1 |
| Python subprocess | /c/msys64/mingw32/bin/gcc.exe | exit code 49 |
| WSL | Ubuntu-22.04 | 未安装 |

### 根因分析
- gcc 编译阶段成功（生成 .s 汇编文件）
- 但 cc1.exe 输出到临时目录后，后续步骤失败
- 链接器无法找到系统库（libmingw32.a, libmsvcrt.a 等）
- 可能是 MSYS2 环境配置问题或路径映射问题

### 建议解决方案
1. **推荐**: 在 Linux 机器上执行编译
2. **备选**: 修复 Windows MSYS2 环境（运行 pacman -Syu 更新）
3. **应急**: 手动复制 Linux 机器编译的 cc_self_host.exe 到 Windows

### 验证状态
- 现有 cc_self_host.exe (Nov 1) 可正常编译小测试程序
- 但无法自举编译 cc.c（缺少 forward-call-fix）
- 需要重建以获得正确的 CC2.CE（预期 81522 字节）

## 附录：自动化验证脚本

### build/verify_bootstrap.sh
一键验证脚本，在 Linux 机器上执行：
```bash
sh build/verify_bootstrap.sh
```

脚本会：
1. 验证 cc.c md5
2. 检查 USER_STACK_SZ 配置
3. 编译 cc_self_host.exe
4. 运行自举编译
5. 验证 CC2.CE md5（应与 HOSTCC2.CE 一致）
6. 运行回归测试
