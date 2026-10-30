/* cpu.s - interrupt stubs + context switching primitives for CatOS
 *
 * The GDT / IDT tables themselves are built in C (intr.c); this file
 * provides the entry stubs they point at, plus the low-level context
 * switch helpers.  Frame layout (low -> high address), see cpu.h:
 *
 *     gs fs es ds | edi esi ebp esp ebx edx ecx eax | intno err |
 *     eip cs eflags [esp ss (ring3 only)]
 *
 * Every stub normalizes the frame: exceptions without a CPU error code
 * push a dummy 0 so the C handler always sees intno+err.
 *
 * NOTE: MinGW prepends an underscore to C symbols, so every symbol
 * exported to / imported from C is written here with a leading '_'.
 */

.set SEL_KDATA, 0x10
.set SEL_TSS,   0x28

/* export everything referenced from C */
.globl _isr_0,  _isr_1,  _isr_2,  _isr_3,  _isr_4,  _isr_5,  _isr_6,  _isr_7
.globl _isr_8,  _isr_9,  _isr_10, _isr_11, _isr_12, _isr_13, _isr_14, _isr_15
.globl _isr_16, _isr_17, _isr_18, _isr_19, _isr_20, _isr_21, _isr_22, _isr_23
.globl _isr_24, _isr_25, _isr_26, _isr_27, _isr_28, _isr_29, _isr_30, _isr_31
.globl _irq_0,  _irq_1,  _irq_2,  _irq_3,  _irq_4,  _irq_5,  _irq_6,  _irq_7
.globl _irq_8,  _irq_9,  _irq_10, _irq_11, _irq_12, _irq_13, _irq_14, _irq_15
.globl _isr_0x80
.globl _isr_exception, _isr_irq, _isr_syscall, _sched_timer_tick
.globl _cpu_switch_to, _cpu_switch_park_and_resume, _cpu_frame_resume
.globl _cpu_paging_enable, _cpu_cr3_read, _cpu_cr3_write, _cpu_cr2_read
.globl _cpu_invlpg, _cpu_gdt_load, _cpu_idt_load, _cpu_idle_halt
.globl _gdt_desc, _idt_desc, _isr_stub_table, _tss

.section .text

/* ---- exceptions 0..31 ------------------------------------------------ */
.macro ISR_NOERR num
_isr_\num\():
    cld
    push $0                 /* dummy error code */
    push $\num
    jmp _isr_exception_body
.endm

.macro ISR_ERR num
_isr_\num\():
    cld
    push $\num              /* CPU already pushed the error code */
    jmp _isr_exception_body
.endm

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
ISR_NOERR 29
ISR_NOERR 30
ISR_NOERR 31

_isr_exception_body:
    push %ds; push %es; push %fs; push %gs
    pushal
    mov $SEL_KDATA, %ax
    mov %ax, %ds; mov %ax, %es; mov %ax, %fs; mov %ax, %gs
    push %esp
    call _isr_exception
    add $4, %esp
    popal
    pop %gs; pop %fs; pop %es; pop %ds
    add $8, %esp            /* drop intno + err */
    iretl

/* ---- IRQ 0..15 -> vectors 0x20..0x2F --------------------------------- */
.macro IRQ num
_irq_\num\():
    cld
    push $0
    push $(0x20+\num)
    jmp _irq_body
.endm

IRQ 1
IRQ 2
IRQ 3
IRQ 4
IRQ 5
IRQ 6
IRQ 7
IRQ 8
IRQ 9
IRQ 10
IRQ 11
IRQ 12
IRQ 13
IRQ 14
IRQ 15

_irq_body:
    push %ds; push %es; push %fs; push %gs
    pushal
    mov $SEL_KDATA, %ax
    mov %ax, %ds; mov %ax, %es; mov %ax, %fs; mov %ax, %gs
    push %esp
    call _isr_irq
    add $4, %esp
    popal
    pop %gs; pop %fs; pop %es; pop %ds
    add $8, %esp
    iretl

/* IRQ 0 (PIT timer) is special: sched_timer_tick() returns the esp of the
 * frame to return into, which may live on a DIFFERENT task's kernel stack
 * (ring3 preemption).  Swapping esp here and falling into the epilogue
 * resumes that task.  The same frame format is used for first task entry
 * (hand-built frames). */
_irq_0:
    cld
    push $0
    push $0x20
    push %ds; push %es; push %fs; push %gs
    pushal
    mov $SEL_KDATA, %ax
    mov %ax, %ds; mov %ax, %es; mov %ax, %fs; mov %ax, %gs
    push %esp
    call _sched_timer_tick
    mov %eax, %esp          /* may switch to another task's saved frame */
    popal
    pop %gs; pop %fs; pop %es; pop %ds
    add $8, %esp
    iretl

/* ---- int 0x80 system call (DPL=3) ------------------------------------ */
_isr_0x80:
    cld
    push $0
    push $0x80
    push %ds; push %es; push %fs; push %gs
    pushal
    mov $SEL_KDATA, %ax
    mov %ax, %ds; mov %ax, %es; mov %ax, %fs; mov %ax, %gs
    push %esp
    call _isr_syscall
    add $4, %esp
    popal
    pop %gs; pop %fs; pop %es; pop %ds
    add $8, %esp
    iretl

/* ---- exported helpers ------------------------------------------------ */

/* void cpu_switch_to(uint32_t *save_esp, uint32_t load_esp)
 * ring0 cooperative switch: saves callee-saved regs on the current stack,
 * stores esp into *save_esp, then resumes the target stack. */
_cpu_switch_to:
    push %ebp; push %ebx; push %esi; push %edi
    mov 20(%esp), %eax      /* &prev->ksp */
    mov %esp, (%eax)
    mov 24(%esp), %esp      /* next->ksp */
    pop %edi; pop %esi; pop %ebx; pop %ebp
    ret

/* void cpu_switch_park_and_resume(uint32_t *save_esp, uint32_t frame_esp,
 *                                 uint32_t cr3, uint32_t kstack_top)
 * Park the CURRENT task (save callee-saved regs + esp into *save_esp)
 * and then enter a ring3 task through its saved ISR frame.  The parked
 * task resumes later via cpu_switch_to, landing right after its call to
 * this function.  Never returns to the caller until then. */
_cpu_switch_park_and_resume:
    cld
    push %ebp; push %ebx; push %esi; push %edi
    mov 20(%esp), %eax      /* &prev->ksp */
    mov %esp, (%eax)
    mov 24(%esp), %eax      /* frame esp  */
    mov 28(%esp), %edx      /* cr3        */
    mov 32(%esp), %ecx      /* kstack top */
    mov %edx, %cr3
    mov %ecx, _tss + 4      /* tss.esp0   */
    mov %eax, %esp
    popal
    pop %gs; pop %fs; pop %es; pop %ds
    add $8, %esp
    iretl

/* void cpu_frame_resume(uint32_t frame_esp, uint32_t cr3, uint32_t kstack_top)
 * Enter (or re-enter) a ring3 task whose saved ISR frame sits at
 * frame_esp on its own kernel stack.  Never returns. */
_cpu_frame_resume:
    cld
    mov 4(%esp), %eax       /* frame esp */
    mov 8(%esp), %edx       /* page directory */
    mov 12(%esp), %ecx      /* kernel stack top (TSS.esp0) */
    mov %edx, %cr3
    mov %ecx, _tss + 4      /* tss.esp0 */
    mov %eax, %esp
    popal
    pop %gs; pop %fs; pop %es; pop %ds
    add $8, %esp
    iretl

/* void cpu_paging_enable(uint32_t kdir_phys): PSE on (4 MiB pages),
 * then PG on with cr3 = kernel directory.  Identity map => safe flip. */
_cpu_paging_enable:
    mov 4(%esp), %eax
    mov %eax, %cr3
    mov %cr4, %eax
    orl $0x00000010, %eax   /* PSE */
    mov %eax, %cr4
    mov %cr0, %eax
    orl $0x80000000, %eax   /* PG */
    mov %eax, %cr0
    jmp 1f
1:  ret

_cpu_cr3_read:
    mov %cr3, %eax
    ret

_cpu_cr2_read:
    mov %cr2, %eax
    ret

_cpu_cr3_write:
    mov 4(%esp), %eax
    mov %eax, %cr3
    ret

_cpu_invlpg:
    mov 4(%esp), %eax
    invlpg (%eax)
    ret

_cpu_gdt_load:              /* (void) - C built the GDT + gdtr first */
    lgdt _gdt_desc
    mov $SEL_KDATA, %ax
    mov %ax, %ds; mov %ax, %es; mov %ax, %fs; mov %ax, %gs
    mov %ax, %ss
    ljmp $0x08, $1f
1:  movw $SEL_TSS, %ax
    ltr %ax
    ret

_cpu_idt_load:
    lidt _idt_desc
    ret

_cpu_idle_halt:
    sti
    hlt
    ret

/* ---- data / bss filled or referenced from C ------------------------- */
.section .data
.p2align 3
_gdt_desc: .word 0
           .long 0
_idt_desc: .word 0
           .long 0

/* table of 49 stub addresses (C sees isr_stub_table[49]):
 * [0..31] exceptions, [32..47] IRQs, [48] = int 0x80 */
.p2align 4
_isr_stub_table:
    .long _isr_0,  _isr_1,  _isr_2,  _isr_3,  _isr_4,  _isr_5,  _isr_6,  _isr_7
    .long _isr_8,  _isr_9,  _isr_10, _isr_11, _isr_12, _isr_13, _isr_14, _isr_15
    .long _isr_16, _isr_17, _isr_18, _isr_19, _isr_20, _isr_21, _isr_22, _isr_23
    .long _isr_24, _isr_25, _isr_26, _isr_27, _isr_28, _isr_29, _isr_30, _isr_31
    .long _irq_0,  _irq_1,  _irq_2,  _irq_3,  _irq_4,  _irq_5,  _irq_6,  _irq_7
    .long _irq_8,  _irq_9,  _irq_10, _irq_11, _irq_12, _irq_13, _irq_14, _irq_15
    .long _isr_0x80

.section .bss
.p2align 8
_tss:      .skip 104            /* tss_t including iomap_base */
