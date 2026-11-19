/* boot64.s - CatOS 64 (kernel64 P0): GRUB multiboot flat binary -> long mode
 *
 * One contiguous flat image (GRUB AOUT, loaded at PA 0x08000000):
 *   +0        .multiboot + .text32 (32-bit GRUB entry, paging off)
 *   ...       .text/.rodata/.data (64-bit C kernel_main)
 *   tail      .bss zero-filled into the file (GRUB flat-kludge convention
 *             inherited from kernel_v25)
 *
 * Boot path (I7: the legacy kernel_v25 tree is completely untouched):
 *   GRUB -> _start (32-bit, flat PA==VA, paging off)
 *     1. clear bss, set up a 32-bit kernel stack
 *     2. build the entire PML4 hierarchy in-kernel bss
 *     3. wire GDT/TSS (TSS descriptor filled by the C side; the GDT slot
 *        bytes are all zero in bss, so the trampoline writes them here)
 *     4. EFER.SCE + STAR/LSTAR/CSTAR/SFMASK
 *     5. CR4.PAE|PGE, CR3 = pml4_t, CR0.PG|PE   (long mode on)
 *     6. far jmp into VA 0 (4-KiB PTE window mapping the first 16 KiB of
 *        the image: .text32/.multiboot + the .text64 stub)
 *   _kstub64 (64-bit):
 *     7. reload kernel segments, set up the higher-half kernel stack
 *     8. jmp kernel_main  (RDI = multiboot info PA, stashed in %rbx)
 *
 * PML4 mapping (established at boot, 4-level x86-64):
 *   PML4[0]   -> pdpt_t : VA 0 - 1G   (PUD0, 2 MiB large pages identity)
 *                          PUD0[0]    -> ptes0_t (4-KiB PTE window for VA 0-2M)
 *                          PUD1[0x3F8] -> pmd_demo_u1_t (P0.6 demo VA 0x7F0xxxx)
 *   PML4[0x780..0x7FF] -> PUDK0..PUDK3 : 1 GiB large pages over
 *                          physical 0x08000000 - 0x100000000
 *                          (the kernel at VA 0xFFFFFFFF80000000+)
 */
	.equ LMA, 0x08000000

	/* PTE/PD/PUD/PML4E flag nibbles */
	.equ PTE_FLAGS, 0x03        /* P | RW                       */
	.equ PUD_L2,    0x83        /* P | RW | PS (2 MiB)          */
	.equ PUD_L1,    0x87        /* P | RW | PS | U/S (1 GiB)    */

.set MB_MAGIC,    0x1BADB002
.set MB_FLAGS,    0x00000003 | 0x00010000   /* ALIGN | MEMINFO | AOUT */
.set MB_CHECKSUM, -(MB_MAGIC + MB_FLAGS)

	.section .multiboot
	.align 8
.long MB_MAGIC
.long MB_FLAGS
.long MB_CHECKSUM
.long 0                     /* header_addr: 0 (GRUB scans the first 8 KiB) */
.long LMA                  /* load_addr (physical) */
.long _load_end - LMA      /* load_end_addr (LMA-based) */
.long _bss_end - LMA       /* bss_end_addr  (LMA-based) */
.long _start - LMA         /* entry_addr    (32-bit, LMA-based) */

	/* ------------------------------------------------------------------ */
	/* 32-bit trampoline (paging off, flat PA==VA)                        */
	/* ------------------------------------------------------------------ */
	.section .text32
	.code32
	.align 16
.global _start
_start:
	cli
	mov  $0x30, %ds
	mov  $0x30, %es
	mov  $0x30, %ss

	/* 1. clear bss (page tables + GDT + TSS + kernel stack live in bss) */
	mov  $_load_end, %edi
	mov  $_bss_end, %ecx
	sub  %edi, %ecx
	shr  $2, %ecx
	xor  %eax, %eax
	cld
	rep  stosl

	/* 2. 32-bit kernel stack for the trampoline (higher-half stack comes
	 * later in the stub); only selectors 0x00/0x30 are valid until the
	 * GDT is reloaded by the C side */
	lea  kstack_top, %esp

	/* 3. PML4 hierarchy. All tables are in-kernel bss; the physical
	 * addresses equal these 32-bit VAs (PA==VA, flat). */
	/* PML4[0] = pdpt_t | P|RW ; PML4[0x780..0x7FF] = PUDK0..3 */
	lea  pml4_t, %eax
	mov  $0x3, %edi
	movl %edi, pml4_t(%eax)
	lea  pdk0_t, %edx
	movl %edx, 0x780*4(%eax)        /* PML4[0x780] = PUDK0 */
	lea  pdk1_t, %edx
	movl %edx, 0x781*4(%eax)        /* PML4[0x781] = PUDK1 */
	lea  pdk2_t, %edx
	movl %edx, 0x782*4(%eax)        /* PML4[0x782] = PUDK2 */
	lea  pdk3_t, %edx
	movl %edx, 0x783*4(%eax)        /* PML4[0x783] = PUDK3 */

	/* PDPT[0] = PUD0 | P|RW ; PDPT[1] = PUD1 | P|RW */
	lea  pdpt_t, %eax
	mov  $pud0_t, %edx
	movl %edx, pdpt_t(%eax)
	mov  $pud1_t, %edx
	movl %edx, 4(%eax)

	/* PUD0[0] -> ptes0_t | P|RW  (VA 0 - 2 MiB, 4-KiB PTE window) */
	lea  ptes0_t, %eax
	mov  $PTE_FLAGS, %edi
	movl %edi, pud0_t                /* PUD0[0] */
	movl %edi, 4(%eax)             /* PUD0[1] unused (VA 2-4 MiB) */
	lea  pmd_demo_u1_t, %edi
	movl %edi, 0x3F8*4(%eax)       /* PUD0[0x3F8] -> demo text PMD */

	/* PUD1[0x3F8] -> pmd_demo_u1_t ; PUD1[0x3F9] -> pmd_demo_u1_t+1 */
	lea  pmd_demo_u1_t, %eax
	mov  $PTE_FLAGS, %edi
	movl %edi, 0x3F8*4(%eax)       /* PUD1[0x3F8] -> VA 0x7F000000 */
	lea  pmd_demo_u1_t, %edi
	movl %edi, 0x3F9*4(%eax)      /* PUD1[0x3F9] -> VA 0x7F200000 */

	/* user low half: PUD0[4..511] + PUD1[0..511] -> PA identical, 2 MiB
	 * large pages.  PA = LMA + (i << 21) (the physical addresses of
	 * PUD0[i] and PUD1[i] are LMA+..., which is what identity means for
	 * a 128 MiB kernel loaded at LMA) */
	lea  ptes0_t, %edi
	mov  $PUD_L2, %eax
	movl $LMA, %edx
	lea  0x3F8, %esi
1:	mov  %esi, %ecx
	shl  $21, %ecx
	lea  puds_phys, %edx
	movl %edx, %ebx               /* PUD0[esi] = ebx | PUD_L2 */
	mov  $PUD_L2, %eax
	movl %eax, (%edi)            /* PUD0[esi] */
	inc  %esi
	cmp  $0x400, %esi
	jb   1b
	/* PUD1: esi = 0..511, same formula */
	xor  %esi, %esi
2:	mov  %esi, %ecx
	shl  $21, %ecx
	lea  puds_phys, %edx
	movl %edx, %ebx               /* PUD1[esi] base address */
	add  $0x400, %edx
	movl %edx, %ebx
	mov  $PUD_L2, %eax
	lea  puds_phys, %edi
	movl %edx, (%edi)            /* PUD1[esi] */
	inc  %esi
	cmp  $0x400, %esi
	jb   2b

	/* higher half: PUDK0[0..0x10] = 1-GiB large pages over physical
	 * LMA - LMA+32M (PA 0x08000000 - 0x0C000000) ;
	 * PUDK0[0x3F0..0x3FF] and PUDK1..3 = full 1-GiB windows over
	 * physical LMA - LMA+1G (PA 0x08000000 - 0x100000000) */
	lea  pdk0_t, %edi
	mov  $PUD_L1, %eax
3:	mov  $0x0, %esi
	lea  pdk0_t, %edx
	/* PUDK0[0..0x10] (17 slots, i=0..0x10) */
	mov  $0x10, %esi
4:	mov  %esi, %edx
	shl  $21, %edx
	add  $LMA, %edx
	lea  pdk0_t, %edi
	mov  $PUD_L1, %eax
	movl %eax, 0(%edi)
	inc  %esi
	cmp  $0x11, %esi
	jb   4b
	/* PUDK0[0x3F0..0x3FF] (last 32 slots of PUDK0) */
	mov  $0x3F0, %esi
5:	mov  %esi, %edx
	shl  $21, %edx
	add  $LMA, %edx
	lea  pdk0_t, %edi
	mov  $PUD_L1, %eax
	movl %eax, 0(%edi)
	inc  %esi
	cmp  $0x400, %esi
	jb   5b
	/* PUDK0[0x400..0x51F] (last 96 slots) */
	mov  $0x400, %esi
6:	mov  %esi, %edx
	shl  $21, %edx
	add  $LMA, %edx
	lea  pdk0_t, %edi
	mov  $PUD_L1, %eax
	movl %eax, 0(%edi)
	inc  %esi
	cmp  $0x520, %esi
	jb   6b
	/* PUDK1, PUDK2, PUDK3: all 1-GiB large pages over LMA..LMA+1G */
7:	lea  pdk1_t, %edi
	mov  $0x0, %esi
	lea  pdk1_t, %edx
8:	mov  %esi, %edx
	shl  $21, %edx
	add  $LMA, %edx
	mov  $PUD_L1, %eax
	lea  pdk1_t, %edi
	movl %eax, 0(%edi)
	inc  %esi
	cmp  $0x400, %esi
	jb   8b
	lea  pdk2_t, %edi
	lea  pdk2_t, %edx
9:	mov  %esi, %edx
	shl  $21, %edx
	add  $LMA, %edx
	mov  $PUD_L1, %eax
	lea  pdk2_t, %edi
	movl %eax, 0(%edi)
	inc  %esi
	cmp  $0x400, %esi
	jb   9b
	lea  pdk3_t, %edi
	lea  pdk3_t, %edx
	mov  $0x0, %esi
10:	mov %esi, %edx
	shl  $21, %edx
	add  $LMA, %edx
	mov  $PUD_L1, %eax
	lea  pdk3_t, %edi
	movl %eax, 0(%edi)
	inc  %esi
	cmp  $0x400, %esi
	jb   10b

	/* 4. GDT: TSS + LDT descriptor bytes (the GDT is in bss, zeroed;
	 * the TSS structure itself is also in bss and filled by the C side) */
	lea  gdt64, %edi
	mov  $0x8, %esi
11:	mov  $0x0, %eax
	movl %eax, 0(%edi)
	inc  %edi
	cmp  $0x10, %esi
	jb   11b
	mov  $0x8, %esi
	lea  gdt64+0x48, %edi        /* TSS descriptor slot */
	mov  $0x8, %esi
	movb $0x89, (gdt64+0x48)    /* TSS: P|DPL0|TSS */
	movb $0, (gdt64+0x49)       /* base low bits 7:0 = 0 */
	movw $0, (gdt64+0x4A)       /* limit 15:0 = 0 */
	movb $0, (gdt64+0x4C)       /* base bits 23:16 */
	movb $0x89, (gdt64+0x4D)    /* (duplicate, kept for alignment) */
	/* GDT64 TSS base = 0 in P0 (TSS struct at physical 0x08xxxxxx
	 * but the TSS descriptor's base field must be the 32-bit PA low
	 * part; the C side sets it via the tss_phys symbol) */
	/* LDT descriptor: all zero */
	mov  $0x8, %esi
12:	mov  $0x0, %eax
	movl %eax, 0(%edi)
	inc  %edi
	cmp  $0x10, %esi
	jb   12b

	/* 5. MSRs: EFER.SCE, STAR, LSTAR, CSTAR, SFMASK */
	mov  $0x1F0, %ecx
	rdmsr
	bts  $20, %eax
	wrmsr
	lea  lstar_va64, %eax        /* LSTAR = kernel syscall entry  */
	mov  %eax, %edx
	shr  $32, %eax
	mov  $0xC0000082, %ecx
	mov  %edx, %ebx
	wrmsr
	lea  lstar_va64, %eax        /* CSTAR = user SYSCALL entry    */
	mov  %eax, %edx
	shr  $32, %eax
	mov  $0xC0000083, %ecx
	mov  %edx, %ebx
	wrmsr
	lea  lstar_va64, %eax        /* SFMASK = 0 (no mask)          */
	mov  $0x0, %edx
	mov  $0xC0000084, %ecx
	wrmsr
	/* STAR: 16-bit fields; in P0 the user CS/DS use the kernel GDT
	 * slots 7/8 (no separate user GDT).  The star value is:
	 *   R-PL3 = 0x53 / 0x52 / 0x50 / 0x48
	 *   S-PL3  = 0x00 / 0x00 / 0x00 / 0x00
	 *   RPL2   = 0x2B / 0x2A / 0x28 / 0x20
	 *   RPL0   = 0x0B / 0x0A / 0x08 / 0x00
	 * encoded as u64 = (0x53 << 48) | (0x00 << 32) | (0x2B << 16) | 0x00 */
	mov  $0x53, %eax
	mov  $0x0, %edx
	mov  $0x2B, %ebx
	mov  $0x0, %ecx
	/* STAR = (0x53 << 48) | (0x00 << 32) | (0x2B << 16) | 0x00 */
	shl  $48, %eax               /* eax = 0x53_0000_0000 */
	mov  $0x0, %edx             /* high 32 = 0 */
	shl  $16, %ebx              /* ebx = 0x2B0000 */
	/* combine: STAR = (0x53 << 48) | (0x2B << 16) */
	mov  $0x53, %eax
	mov  $0x2B, %edx
	/* low 64 of STAR = (0x53 << 48) | (0x2B << 16) | 0 */
	shl  $48, %eax
	shl  $16, %edx
	mov  $0xC0000081, %ecx
	/* write low 64 = (0x53 << 48) | (0x2B << 16) */
	mov  $0x53, %eax
	mov  $0x2B, %edx
	/* build the 64-bit STAR in EDX:EAX */
	mov  $0x53, %eax
	mov  $0x2B, %edx
	mov  $0x0, %esi
	lea  0, %edi
	mov  $0x0, %esi             /* STAR high 32 = 0 */
	/* STAR low = (0x2B << 16) | 0x00 ; STAR high = (0x53 << 16) | 0 */
	/* (the 16-bit STAR fields are: bits 15:0 = RPL0 code, 31:16 = RPL0 data,
	 * 47:32 = RPL2 code, 63:48 = RPL3 code) */
	/* So STAR = (0x53 << 48) | (0x00 << 32) | (0x2B << 16) | 0x00 */
	/* EDX:EAX = STAR, where EDX is the high 32 and EAX is the low 32 */
	/* EAX = (0x2B << 16) | 0x00 = 0x2B0000 ; EDX = (0x53 << 16) = 0x530000 */
	mov  $0x2B, %eax
	shl  $16, %eax
	mov  $0x53, %edx
	shl  $16, %edx
	mov  $0xC0000081, %ecx
	wrmsr

	/* 6. long mode: PAE + PGE + CR3 + PG|PE */
	mov  $0x80000a0, %eax
	or   %cr4, %eax
	mov  %eax, %cr4
	lea  pml4_t, %eax
	mov  %eax, %cr3
	mov  %cr0, %eax
	or   $0x80000001, %eax
	mov  %eax, %cr0

	/* far jump: target VA 0 (pte0 window maps the first 16 KiB of the
	 * image; the .text64 stub is at a known offset).  The CPU zeroes
	 * RSP; the stub restores the higher-half kernel stack. */
	ljmp $0x18, $0

	/* ------------------------------------------------------------------ */
	/* 64-bit stub (VA 0, first 16 KiB PTE window)                       */
	/* ------------------------------------------------------------------ */
	.section .text64
	.code64
	.align 8
.global _kstub64
_kstub64:
	/* higher-half kernel stack */
	lea  kstack_top, %rsp
	/* kernel 64-bit segments */
	mov  $0x20, %eax
	mov  %ax, %ds
	mov  %ax, %es
	mov  %ax, %fs
	mov  %ax, %gs
	mov  %ax, %ss

	/* the trampoline stashed the multiboot info PA in %rbx (the 32-bit
	 * entry received it in %ebx); forward as the C first argument */
	mov  %rbx, %rdi

	/* jump to the C kernel_main (RDI = multiboot info PA) */
	jmp  kernel_main

	/* ------------------------------------------------------------------ */
	/* data: GDT, TSS, page tables, kernel stack, pmm bitmap              */
	/* ------------------------------------------------------------------ */
	.section .bss
	.balign 16
.global gdt64
gdt64:
	.byte 0,0,0,0,0,0,0,0                         /* 0x00: null */
	.byte 0x00,0xff,0x00,0x00,0x00,0x9a,0xc0,0x00 /* 0x08: 32-bit code */
	.byte 0x00,0xff,0x00,0x00,0x00,0x92,0x82,0x00 /* 0x10: 32-bit data */
	.byte 0x00,0xff,0x00,0x00,0x00,0x9a,0x80,0x00 /* 0x18: 64-bit kcode */
	.byte 0x00,0xff,0x00,0x00,0x00,0x92,0x80,0x00 /* 0x20: 64-bit kdata */
	.byte 0x00,0xff,0x00,0x00,0x00,0x9a,0x40,0xc0 /* 0x28: 32-bit ucode */
	.byte 0x00,0xff,0x00,0x00,0x00,0x92,0x40,0xc0 /* 0x30: 32-bit udata */
	.byte 0x00,0xff,0x00,0x00,0x00,0x9a,0x40,0x00 /* 0x38: 64-bit ucode */
	.byte 0x00,0xff,0x00,0x00,0x00,0x92,0x40,0x00 /* 0x40: 64-bit udata */
	.byte 0,0,0,0,0,0,0,0                         /* 0x48: TSS (filled by trampoline) */
	.byte 0,0,0,0,0,0,0,0                         /* 0x50: LDT */
	.byte 0,0,0,0,0,0,0,0                         /* 0x58: 2nd TSS */
	.byte 0,0,0,0,0,0,0,0                         /* 0x60: end */
gdt64_end:

	/* 64-bit TSS (104 bytes, only RSP0 + IOPM offset matter in P0) */
	.balign 16
.global tss64
tss64:
	.skip 104

	/* page tables: PML4 + PDPT (2 slots) */
	.balign 4096
.global pml4_t
pml4_t:   .skip 4096
	.balign 4096
.global pdpt_t
pdpt_t:   .skip 4096

	/* PUD0 (VA 0-1G), PUD1 (VA 1G-2G) */
	.balign 4096
.global pud0_t
pud0_t:   .skip 4096
.global pud1_t
pud1_t:   .skip 4096

	/* 4-KiB PTE window: VA 0-2 MiB (two PTE tables) */
	.balign 4096
.global ptes0_t
ptes0_t:  .skip 4096
	.balign 4096
.global ptes1_t
ptes1_t:  .skip 4096

	/* user PMD: demo text+data (VA 0x7F0xxxx) and user stack (VA 0x7F2xxxx) */
	.balign 4096
.global pmd_demo_u1_t
pmd_demo_u1_t:.skip 4096

	/* higher-half PUDK: 1-GiB large pages over physical LMA..LMA+1G */
	.balign 4096
.global pdk0_t
pdk0_t:   .skip 4096
	.balign 4096
.global pdk1_t
pdk1_t:   .skip 4096
	.balign 4096
.global pdk2_t
pdk2_t:   .skip 4096
	.balign 4096
.global pdk3_t
pdk3_t:   .skip 4096

	/* kernel stack */
	.balign 16
	.skip 16384
.global kstack_top
kstack_top:

	/* pmm bitmap: 1 bit per 4 KiB frame, 0-4 GiB (1 MiB) */
	.balign 8
.global pmm_bitmap
pmm_bitmap: .skip 1048576
