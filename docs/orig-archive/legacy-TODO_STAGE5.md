# Stage5 待办事项 (2026-11-05)

## 已完成
- [x] 修复 Windows mingw32 工具链 (PATH 配置)
- [x] 编译 cc_self_host_new.exe
- [x] 回归测试 6/6 通过
- [x] 自举编译 cc.c -> CC2_NEW.CE

## 待完成
- [ ] 调查 CC2_NEW.CE (82586B) vs HOSTCC2.CE (81522B) 大小差异
- [ ] 如有必要，使用与原编译环境相同的 gcc 版本重新编译
- [ ] QEMU 实机回归测试
- [ ] 验证 CC2.CE 自举 (CC2 -> CC3 字节级一致)
- [ ] 重建内核 ISO (USER_STACK_SZ=0x20000)

## 技术笔记
- 工具链位置：/c/msys64/mingw32/bin/gcc.exe (Rev4, 16.2.0)
- 原编译环境：未知 (可能为 Linux + 不同版本 gcc)
- 环境变量：CCDEV_ROOT 必须设置指向源码目录
