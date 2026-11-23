; demo/hello.s - P0.6 demo: ELF64 ring3 program, linked at 0x40000000
; proves both entry paths: int 0x80 (IDT gate) and syscall/sysret (STAR/LSTAR)
BITS 64

SECTION .text
global _start
_start:
	lea	rdi, [rel msg1]
	mov	rsi, msg1_len
	mov	rax, 1			; sys_write
	int	0x80

	lea	rdi, [rel msg2]
	mov	rsi, msg2_len
	mov	rax, 1			; sys_write
	syscall				; fast path through IA32_LSTAR

	lea	rdi, [rel msg3]
	mov	rsi, msg3_len
	mov	rax, 1
	int	0x80			; proves we came back to ring3 after sysret

	mov	rax, 2			; sys_exit
	syscall
.hang:
	jmp	.hang

SECTION .rodata
msg1:
	db "hello from ELF64 ring3 via int 0x80 (CatOS64 P0.6)", 10
msg1_len equ $ - msg1
msg2:
	db "second line via syscall/sysret MSR path", 10
msg2_len equ $ - msg2
msg3:
	db "back in ring3 after sysret", 10
msg3_len equ $ - msg3
