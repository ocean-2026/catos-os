# kernel64 P0 验收报告（C-P0.1 ~ C-P0.6）

日期：2026-11-23
范围：`docs/全局待办总表.md` §5.1（线 C / 通用 OS 战略 P0 — 64 位地基）

## 结论

P0 六项全部落地并跑通，具备"GRUB → long mode → C 内核 → ring3 ELF64 → syscall 返回 → REPL"
的完整闭环。原先 `kernel64/` 只有一份半成品 `boot/boot64.s` 草稿（GDT 写字节错乱、页表循环不
写值、STAR 构造冗余），本次按"可验证优先"整树重写。

验收命令与现场：

```
qemu-system-x86_64 -cdrom build/catos64.iso -m 3840 -smp 4 -serial stdio -display none -no-reboot
```

```
CatOS64 P0 - long mode bring-up
  boot     : GRUB multiboot (ELF), 32-bit trampoline -> long mode
  paging   : PML4/PDPT/PD, identity 0-1 GiB (2 MiB pages)
             user window 0x40000000 (code) / 0x40200000 (stack)
  kernel   : 0x0000000008000000 .. 0x00000000080310f8
  multiboot: flags=0x0000000000001a67 mem_upper(KiB)=3144576 mmap_len=192
  idt/pic  : 32 exception stubs + IRQ0 1 kHz + int 0x80 (DPL3)
  syscall  : IA32_LSTAR/SFMASK + EFER.SCE armed
  pmm      : total frames=1048576 used=262483 free=786093
  usermap  : 0x40000000 -> 0x0000000000200000, 0x40200000 -> 0x0000000000400000
[k64] entering ring3 at 0x0000000040000000 (image 5032 bytes)
[ring3] hello from ELF64 ring3 via int 0x80 (CatOS64 P0.6)
[ring3] second line via syscall/sysret MSR path
[ring3] back in ring3 after sysret
[k64] ring3 demo exited

[k64] back in kernel (ticks=11)
catos64> 
```

完整（未截断）串口日志见 `kernel64/evidence/p0-serial-3840mb-4cpu.log`。

## 逐项交付

| 项 | 实现 | 验证点 |
|----|------|--------|
| C-P0.1 | `boot/boot32.s`：multiboot 头、清 bss、存 mb_info、建表、`EFER.LME`、`CR0.PG`、`lgdt`、far jmp `0x08:_k64` | 串口 banner 打出 multiboot flags/mem_upper/mmap_len |
| C-P0.2 | PML4[0]→PDPT（US 宽松 0x07），PDPT[0]→identity PD（512×2 MiB，0–1 GiB），PDPT[1]→用户 PD | 内核在 VA=PA 下运行；用户窗口 0x40000000 可进 |
| C-P0.3 | `src/pmm64.c`：1 bit/4 KiB、4 GiB 位图、按 mmap type=1 释放、保留 low 1 MiB/内核镜像/mb 结构；`pmm_alloc`、`pmm_alloc_2m` | 3840 MB：used=262483、free=786093 与 mem_upper 一致 |
| C-P0.4 | `src/idt64.c` + `boot/isr64.s` + `src/serial.c`：256 项 IDT、异常 0–31（两条路径：无错误码补 0 / 带错误码）、IRQ 32–47、`int 0x80` DPL3、TSS(rsp0)、PIC/PIT 1 kHz、PANIC 打印 vec/err/rip | PIT 计数增长；故意错误的访问打出 `PANIC: exception vec=14 err=... rip=...` |
| C-P0.5 | `src/sys64.c` + `boot/isr64.s:syscall_entry`：STAR=(0x20<<48)\|(0x08<<32)、LSTAR=syscall_entry、SFMASK=0、EFER.SCE=1；`sysret` 回环 3 | demo 里 `syscall` 写一行、再用 `int 0x80` 写一行证明已回到 ring3 |
| C-P0.6 | `src/elf64.c` + `boot/demo_inc.s`（`incbin build/hello.elf`）+ `demo/hello.s`/`demo/hello.ld` | ELF64 PT_LOAD 拷贝到用户物理帧、`enter_user` iretq 进 ring3、exit 后回 `catos64>` |

## 关键设计取舍

1. **不用 higher-half**：原 `k64.h` 草稿写 VA `0xFFFF80000000`，但 P0 阶段没有任何收益，反而要
   维护两份映射并让 C 代码区分 `KPHYS`/`VA`。改为 identity 0–1 GiB + 一个 user 窗口，寄存器级
   行为可预测。higher-half 留到 P1 引入 VMA/物理内存分区时再迁移（届时只改 trampoline 与链接基址）。
2. **用户窗口用 PMM 分配的 2 MiB 帧，不写死物理地址**：第一版写死 `0x3E000000`（≥1 GiB 才有 RAM），
   `-m 512` 下页表有值但物理页根本不存在，ring3 一执行就 `#PF err=0x5`。现在 `pmm_alloc_2m()` 在
   `user_window_init()` 里取帧并回填 `pd_user_t[0]`/`pd_user_t[1]`，再重载 CR3。
3. **先 `int 0x80` 后 `syscall`**：`int 0x80` 由中断门自动切 TSS.RSP0，能先把 ring3 通路跑通；
   `syscall` 不切栈，需手写 `kernel_rsp_save` 栈切换 + `sysret` 返回，两条路径都保留。

## 踩坑记录（都在本次修复）

| 症状 | 根因 | 修复 |
|------|------|------|
| long mode 后立刻 `#UD`/triple fault，QEMU 报 `CR4=0x20` | GRUB 32 位环境没开 SSE，且 `CR0.EM/TS` 未清，C 代码里 `movq %rax,%xmm7` 直接 #UD | `CR4 |= PAE|OSFXSR|OSXMMEXCPT`，`CR0 &= ~(EM|TS)` 且 `|= MP`，且**先 idt 后 pmm**，否则异常被静默成 triple fault |
| `mb_info` 里的 mmap 地址失效 | 存 `mb_info` 早于清 bss，被 `rep stosd` 冲掉 | 清完 bss 再 `mov [mb_info], ebx` |
| PMM 统计循环"卡死"，每秒只推进几十帧 | IRQ0 桩只 `push 0`、`call` 后直接 `iretq`，被中断的 `rax/rcx/rdx` 全被 C 函数冲掉，循环状态被持续破坏 | IRQ/ISR/`int 0x80` 桩统一 15 个 GPR 全保存（`PUSHREGS/POPREGS`） |
| ring3 第一条指令 `#PF err=0x5` | ①用户窗口写死 PA 超出实际 RAM；②PML4/PDPT 中间层没置 U/S | ①改 PMM 分配（见上）；②中间层 `0x07`，叶子 `0x87` |
| `sysret` 后 `#GP err=0x30` | `SYSRET` 规定 CS=`STAR[63:48]+16`、SS=`+8`，而 GDT 里 ucode 在 udata 前面 | GDT 重排为 `0x28 udata(DPL3)`、`0x30 ucode(DPL3)`，`STAR[63:48]=0x20`，选择子 0x2B/0x33 |
| ELF 段被判定 "outside user window" | 用 `ld -Ttext=0x40000000` 起步，段被页对齐拉低 | 改用 `demo/hello.ld`（`. = 0x40000000`），并把段校验放宽为 `[0x40000000, +4 MiB)` |
| NASM 把 `sysretq` 当标签 | NASM 版本不支持该助记符，只报 "label alone on a line" 警告，代码静默变成 fall-through | 写成 `db 0x48,0x0F,0x07` |

## 已知边界（不属 P0 验收范围）

- **SMP 只跑 BSP**：`-smp 4` 下其余 3 个 AP 停在 real mode，没做 INIT/SIPI、AP 的 GDT/IDT/栈。
- **物理内存上限 4 GiB**：位图按 4 GiB 静态开（128 KiB），>4 GiB 需扩到动态位图/分段池。
- **无 2 MiB 大页分配器**，无 NUMA，无 per-CPU 缓存。
- **纯 identity 映射**：超过 1 GiB 的 RAM 目前不可达（P0 只用 low 1 GiB）。
- **旧树回归 12/12 未跑**：本轮未在 `kernel_v25` 上执行回归脚本（相关改动属于一天级任务，由并行子代理处理）。
- **无中断优先级/软中断**：IRQ 全在 PIC 下、只开 IRQ0；APIC 属 P1（C-P1.5）。

## 复现步骤

```bash
# WSL Ubuntu 24.04，已装 gcc/nasm/ld/qemu/grub-pc-bin/xorriso/mtools
cd /mnt/d/CatOs/kernel64
sh build.sh          # nasm + gcc -m64 -ffreestanding + ld -T boot/k64.ld
sh mkiso.sh          # grub-mkrescue -> build/catos64.iso
qemu-system-x86_64 -cdrom build/catos64.iso -m 3840 -smp 4 -serial stdio -display none -no-reboot
```

调试用：`-d int,cpu_reset -D /tmp/q.log`，再 `grep 'v=' /tmp/q.log` 看异常向量与 RIP。

## 下一步（P1 入口）

1. `user_window_init` 改成 VMA 建图（按 PT_LOAD 段权限分别 map），为 P2 的完整 ELF64 加载器铺路。
2. 引入 higher-half（`VA 0xFFFF80000000` → PA 0）与物理窗口，解锁 >1 GiB 内存。
3. AP bring-up：INIT/SIPI + per-CPU GDT/IDT/栈 + APIC 定时器（并入 C-P1.5）。
4. 把 `sys_call` 从裸指针改成 `copy_from_user`（与 §E3 的 static 缓冲私有化对齐）。
