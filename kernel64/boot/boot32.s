; boot32.s - CatOS64 P0.1/P0.2 boot: GRUB multiboot ELF -> long mode
;
; Layout: linked at PA 0x08000000, identity mapped 0-1 GiB with 2 MiB pages.
; A second PDPT entry maps a user window at VA 0x40000000 (code) /
; 0x40200000 (stack) over two fixed 2 MiB frames, with U/S=1.
;
; 32-bit trampoline (paging off) -> build tables -> EFER.LME -> CR0.PG
; -> lgdt + far jmp 0x08:_k64 (L=1 code selector) -> 64-bit C kernel.

BITS 32

MB_MAGIC	equ 0x1BADB002
MB_FLAGS	equ 0x00000003		; ALIGN | MEMINFO
MB_CHECK	equ -(MB_MAGIC + MB_FLAGS)

SECTION .multiboot
ALIGN 4
	dd MB_MAGIC
	dd MB_FLAGS
	dd MB_CHECK

SECTION .text
global _start
global gdt64, gdt64_ptr, stack_top64, pd_user_t
extern kmain64
extern __bss_start, __bss_end

_start:
	cli
	mov	esp, stack_top32

	; zero bss (page tables, GDT, TSS, stacks, pmm bitmap)
	mov	edi, __bss_start
	mov	ecx, __bss_end
	sub	ecx, edi
	shr	ecx, 2
	xor	eax, eax
	cld
	rep	stosd
	mov	[mb_info], ebx
	mov	esi, ebx

	; ---- PML4 / PDPT ----
	mov	eax, pml4_t
	mov	dword [eax], pdpt_t + 0x07	; PML4[0] -> PDPTs (US permissive)
	mov	eax, pdpt_t
	mov	dword [eax], pd_t + 0x03	; PDPT[0] -> identity PD
	mov	dword [eax + 8], pd_user_t + 0x07 ; PDPT[1] -> user PD (US)

	; ---- identity 0-1 GiB: 512 x 2 MiB pages ----
	mov	edi, pd_t
	xor	ecx, ecx
.id_loop:
	mov	ebx, ecx
	shl	ebx, 21
	or	ebx, 0x83			; P | RW | PS
	mov	[edi + ecx * 8], ebx
	inc	ecx
	cmp	ecx, 512
	jb	.id_loop

	; ---- user window: VA 0x40000000 / 0x40200000 ----
	mov	eax, pd_user_t
	mov	dword [eax], 0x3E000000 + 0x87	; P|RW|US|PS
	mov	dword [eax + 8], 0x3F200000 + 0x87

	; ---- PAE + CR3 + EFER.LME + PG ----
    mov	eax, cr4
    or	eax, (1 << 5) | (1 << 9) | (1 << 10)	; PAE | OSFXSR | OSXMMEXCPT
    mov	cr4, eax
    mov	eax, cr0
    and	eax, ~((1 << 2) | (1 << 3))		; clear EM | TS
    or	eax, (1 << 1)				; MP
    mov	cr0, eax

	mov	eax, pml4_t
	mov	cr3, eax

	mov	ecx, 0xC0000080			; IA32_EFER
	rdmsr
	or	eax, (1 << 8)			; LME
	wrmsr

	mov	eax, cr0
	or	eax, 0x80000001			; PG | PE
	mov	cr0, eax

	lgdt	[gdt64_ptr]

	jmp	0x08:_k64

BITS 64
_k64:
	mov	ax, 0x10
	mov	ds, ax
	mov	es, ax
	mov	fs, ax
	mov	gs, ax
	mov	ss, ax
	mov	rsp, stack_top64
	mov	edi, dword [mb_info]
	call	kmain64
.hang:
	hlt
	jmp	.hang

; ---------------------------------------------------------------------
; GDT layout (SYSRET wants user CS = X+16, user SS = X+8, X = 0x20):
;   0x00 null | 0x08 kcode64 | 0x10 kdata64 | 0x18 TSS(16 B spans 0x18-0x28)
;   0x28 ucode64 DPL3 | 0x30 udata64 DPL3
; ---------------------------------------------------------------------
SECTION .data
ALIGN 16
gdt64:
	dq 0x0000000000000000			; 0x00 null
	dq 0x00209A0000000000			; 0x08 kcode64  (L=1)
	dq 0x0000920000000000			; 0x10 kdata64
	; 0x18 64-bit TSS descriptor (16 bytes) - filled by C (idt64_init)
	dq 0x0000000000000000
	dq 0x0000000000000000
	dq 0x0000F20000000000			; 0x28 udata64  DPL3 (SYSRET SS)
	dq 0x0020FA0000000000			; 0x30 ucode64  DPL3 (SYSRET CS)
gdt64_end:

gdt64_ptr:
	dw gdt64_end - gdt64 - 1
	dq gdt64

; ---------------------------------------------------------------------
SECTION .bss
ALIGN 4096
pml4_t:		resq 512
ALIGN 4096
pdpt_t:		resq 512
ALIGN 4096
pd_t:		resq 512
ALIGN 4096
pd_user_t:	resq 512
ALIGN 4096
stack_bottom32:
	resb 16384
stack_top32:
	; 64-bit kernel stack (16 KiB)
	resb 16384
stack_top64:

ALIGN 8
mb_info:	resd 1








