# CatOS 在 VMware 中启动

## 结论先行

**不需要 grub-mkrescue，也不需要重新制作启动盘。** `kernel_v25/catos.iso`
本来就是 **GRUB isohybrid 双格式引导镜像**：

- 扇区 0 有 GRUB 的 isohybrid MBR 引导代码（`eb 63 90` 签名）→ 可当硬盘引导
- 带标准 El Torito 引导目录 → 可当光盘引导

已在 QEMU（SeaBIOS，与 VMware BIOS 同源）实测：把 `catos.iso` 挂为 IDE
硬盘 `-boot order=c`，GRUB 菜单 → CatOS shell（`root@catos:/$`）完整启动。
VMware 用的是同类 BIOS 仿真，这两条引导路径都支持。

之前"VMware 卡住"的真正原因：`tests/qemu_disk_*.img` 和
`kernel_v25/catos_disk.img` 是**纯数据盘**——MBR 引导区全零（只有分区表 +
0x55AA 签名，见 `kernel_v25/build/mkdisk.py:write_metadata()`），根本不含
引导代码，任何机器引导它都会卡死。引导介质一直是 ISO，不是磁盘镜像。

## 文件

| 文件 | 作用 |
|---|---|
| `CatOS.vmx` | 虚拟机配置，双击或用 VMware 打开即可 |
| `catos-boot.vmdk` | 指向 `../kernel_v25/catos.iso` 的 raw 描述符（备用：当硬盘引导时用） |
| `catos-data.vmdk` | 指向 `../kernel_v25/catos_disk.img` 的 raw 描述符（FAT32 数据盘） |

## 启动步骤（推荐：光驱 El Torito 引导）

1. 打开 `CatOS.vmx`（VMware Workstation / Player）。
2. 布局与 QEMU 回归完全一致，内核驱动扫描逻辑匹配：
   - **IDE 0:0** = `catos-data.vmdk`（FAT32 数据盘，primary master ——
     内核 `ata_identify()` 取第一个能 IDENTIFY 的盘挂 FAT32，必须是它）
   - **IDE 1:0** = `catos.iso` 虚拟光驱（El Torito 引导）
3. `bios.bootOrder = "cdrom"`，开机即进 GRUB → CatOS shell。
4. `/bin` 应用来自 IDE 0:0 的数据盘；没有数据盘时内核用内嵌应用兜底。

## 备选：纯硬盘引导（不挂光驱）

把 `catos-boot.vmdk` 挂到 **IDE 1:0**（secondary master），数据盘留在
IDE 0:0，`bios.bootOrder = "hdd"`。VMware BIOS 会先试 IDE 0:0（数据盘
无引导代码，失败）再落到 IDE 1:0 的 isohybrid MBR → GRUB → CatOS。
已验证该 MBR 在 QEMU SeaBIOS 下可引导，VMware 同样应支持。

## 已在 QEMU 完成的验证

- `catos.iso` 当 IDE 硬盘引导：GRUB 菜单出现、自动引导、进入
  `root@catos:/$`（截图 `tests/qemu_shots/hdbootA.png` / `hdbootB.png`）。
- about 显示 `Bootloader: GRUB 2 (ISO9660 / El Torito)`，
  `Runs on: QEMU / VMware / Bochs / real PC`。
- PCI 探测正常（i440FX 0x1237 —— VMware I/O 虚拟化同样是 440FX 芯片组）。

## 注意

- `qemu_disk_*.img`（tests/ 下）是回归用一次性数据盘，别拿去当引导盘。
- VMware 虚拟网卡与 QEMU pcnet 不同；CatOS 目前网络栈探测不到网卡只会
  提示 NOT FOUND，不影响其他功能。
- **显卡（v2.20 起）**：VMware 默认 SVGA II（`15AD:0405`）已支持，
  `vbe` / `vbe24` / `gfx24` 都能进 800x600 真彩；`svgainfo` 可查
  ID / BAR / FIFO 刷新计数。旧版报 `no VBE/Bochs-dispi` 属内核版本过旧。
  本仓库的自动验证在 QEMU `-vga vmware`（同一 PCI 设备）上逐像素通过；
  真实 Workstation 请开机按 `docs/CatOS-系统内测试文档.md` §3.4 复测。
- 若 VMware 版本较老（硬件版本 <16），把 `virtualHW.version` 与
  `ddb.virtualHWVersion` 调低到对应数字即可。

## 实测记录（2026-11-12）

- 本机 VMware Workstation Pro 17.6.1（安装于 D:/mine/，vmrun/vmcli 同目录）。
- VM 已注册进库存（inventory.vmls，vmlist4 槽位，displayName "CatOS"），
  `vmrun list` 显示 running，vmware.log 确认 720x400（80x25 文本模式）MKS
  窗口创建 —— 引导链 El Torito → GRUB → CatOS shell 工作正常。
- 旧 VM（CatOS / CatOS_Clear / catos_clear）已从库存注销并删除目录；
  D:/CatOS.vhd 已删；kali VM 未动。库存备份：
  %APPDATA%\VMware\inventory.vmls.bak-20261112
- 注意：catos_clear 的 vmdk 曾直接引用仓库 tests/qemu_disk_user.img，
  因此删除用 unregister+手动删目录，未用 vmrun deleteVM，仓库文件无损。
