CatOS v2.82 发布包（对应 git tag v2.82 = 35f666b / main = 4554b63）

文件：
  catos-v2.82.iso        标准版光盘镜像（12,267,520 B），开机默认 gfx24 块化桌面
  catos-v2.82-test.iso   自启动版（GUI_AUTOSTART=1），开机直进桌面，适合快速验收
  kernel-v2.82.bin       裸内核镜像（1,541,576 B），供引导链/调试使用
  SHA256SUMS.txt         三个产物的 sha256

校验：
  Windows:  Get-FileHash .\catos-v2.82.iso -Algorithm SHA256
  Linux:    sha256sum -c SHA256SUMS.txt

在 VMware 中使用：把 coreos 镜像指到 catos-v2.82.iso 即可（旧版 VMX 无需改其它项）。
QEMU 参考：qemu-system-i386 -m 256 -cdrom catos-v2.82.iso -boot d
