# CatOS 易错点台账

CatOS[易错点] 合成顺序：父容器 `z` 大于子块 `z` 时，子块被父 panel 底色覆盖（桌面只剩第一项）。修法给父子同 `z`，靠先序"父底先、子内容后"；排查先用串口打几何确认布局，再看 `compose.c`/`paint.c`。
CatOS[易错点] `ser_dec` 是各模块各自一份 static 副本，跨模块引用会 `undefined reference`；串口打点统一用 `serial_log.h` 的 `ser_hex(tag, v)`。
CatOS[易错点] `BLK_ICON` 在 `layout.c` 的 `leaf_natural_w/h()` 里原本返回 0，新建图标块必须补分支，否则尺寸为 0、不显示。
CatOS[易错点] QEMU 测试脚本靠 `shutil.which` 找可执行文件，必须 `export PATH=/home/user/bin:$PATH`，否则 `qemu-system-i386 not found`；杀进程用 `pkill -9 -x qemu-system-i386`，别用 `pkill -f qemu`（会误杀自己）。
CatOS[易错点] gfx24 默认后端下 `read_screen` 读的是 VGA 文本内存，开机后必须先 `force_text(s, settle=10)` 再 `wait_for_prompt`，否则命令全被丢弃。
