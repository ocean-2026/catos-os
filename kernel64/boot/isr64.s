; isr64.s - CatOS64 P0.4/P0.5: exception stubs, PIT IRQ, int 0x80 + syscall MSR path
BITS 64

extern exc_dispatch
extern irq_dispatch
extern sys_dispatch
extern kernel_rsp_save
extern repl_resume

SECTION .text

; save/restore every GPR the C handler may clobber
%macro PUSHREGS 0
	push	rax
	push	rbx
	push	rcx
	push	rdx
	push	rsi
	push	rdi
	push	rbp
	push	r8
	push	r9
	push	r10
	push	r11
	push	r12
	push	r13
	push	r14
	push	r15
%endmacro

%macro POPREGS 0
	pop	r15
	pop	r14
	pop	r13
	pop	r12
	pop	r11
	pop	r10
	pop	r9
	pop	r8
	pop	rbp
	pop	rdi
	pop	rsi
	pop	rdx
	pop	rcx
	pop	rbx
	pop	rax
%endmacro

%macro ISR_NOERR 1
global isr%1
isr%1:
	push	qword 0			; fake error code
	push	qword %1			; vector
	jmp	isr_common
%endmacro

%macro ISR_ERR 1
global isr%1
isr%1:
	push	qword %1			; vector (error code already on stack)
	jmp	isr_common
%endmacro

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29
ISR_ERR   30
ISR_NOERR 31

; isr_common: [rsp] = vector, [rsp+8] = error code, [rsp+16] = rip
isr_common:
	mov	rdi, [rsp]			; vec
	mov	rsi, [rsp + 8]			; err
	mov	rdx, [rsp + 16]			; rip
	call	exc_dispatch			; does not return
	iretq

; ---------------------------------------------------------------------
%macro IRQ 2
global irq%1
irq%1:
	PUSHREGS
	mov	rdi, %2
	call	irq_dispatch
	POPREGS
	iretq
%endmacro

IRQ 0, 32
IRQ 1, 33
IRQ 2, 34
IRQ 3, 35
IRQ 4, 36
IRQ 5, 37
IRQ 6, 38
IRQ 7, 39
IRQ 8, 40
IRQ 9, 41
IRQ 10, 42
IRQ 11, 43
IRQ 12, 44
IRQ 13, 45
IRQ 14, 46
IRQ 15, 47

; ---------------------------------------------------------------------
; enter_user(rdi = entry, rsi = user stack top): iretq into ring3
global enter_user
enter_user:
	mov	[kernel_rsp_save], rsp
	push	qword 0x2B			; SS = user data
	push	rsi				; RSP
	push	qword 0x202			; RFLAGS (IF)
	push	qword 0x33			; CS = user code (RPL3)
	push	rdi				; RIP
	mov	ax, 0x2B
	mov	ds, ax
	mov	es, ax
	mov	fs, ax
	mov	gs, ax
	iretq

; int 0x80: rax = call number, rdi/rsi = args
global int80_entry
int80_entry:
	PUSHREGS
	mov	r10, rdi			; arg0 (buf)
	mov	r11, rsi			; arg1 (len)
	mov	rdi, rax			; snum
	mov	rsi, r10
	mov	rdx, r11
	call	sys_dispatch			; 0 = return to user, 1 = user exited
	cmp	rax, 1
	je	.user_exit
	POPREGS
	iretq
.user_exit:
	POPREGS
	mov	rsp, [kernel_rsp_save]
	jmp	repl_resume

; ---------------------------------------------------------------------
; SYSENTER-style fast path armed through IA32_STAR/LSTAR: `syscall` from
; ring3 lands here with rcx = return rip, r11 = rflags, no stack switch.
global syscall_entry
syscall_entry:
	mov	[user_rsp_save], rsp
	mov	rsp, [kernel_rsp_save]
	push	rcx				; user rip
	push	r11				; user rflags
	push	rax
	push	rdx
	push	rsi
	push	rdi
	push	r10
	mov	r10, rdi			; arg0 (buf)
	mov	r11, rsi			; arg1 (len)
	mov	rdi, rax			; snum
	mov	rsi, r10
	mov	rdx, r11
	call	sys_dispatch
	cmp	rax, 1
	je	syscall_exit
	pop	r10
	pop	rdi
	pop	rsi
	pop	rdx
	pop	rax
	pop	r11
	pop	rcx
	mov	rsp, [user_rsp_save]
	db	0x48, 0x0F, 0x07		; sysretq
syscall_exit:
	mov	rsp, [kernel_rsp_save]
	jmp	repl_resume

SECTION .bss
ALIGN 8
user_rsp_save:	resq 1




