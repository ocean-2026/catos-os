#ifndef CPU_H
#define CPU_H

/* cpu.h - GDT / TSS / IDT / interrupt frames / context switching
 *
 * CatOS五大支柱的基础硬件层:
 *   - GDT with ring0 + ring3 segments and a TSS (syscall kernel stack)
 *   - IDT with exception gates (0..31), IRQ gates (0x20..0x2F) and the
 *     int 0x80 system-call gate (DPL=3)
 *   - a uniform interrupt frame layout pushed by the asm stubs
 *   - cooperative ring0 context switch + ring3 resume-frame trampoline
 */

#include <stdint.h>

/* ---- segment selectors ---- */
#define SEL_KCODE 0x08
#define SEL_KDATA 0x10
#define SEL_UCODE 0x1B   /* ring3, GDT index 3 */
#define SEL_UDATA 0x23   /* ring3, GDT index 4 */
#define SEL_TSS   0x28

/* ---- interrupt frame (pushed by the stubs in cpu.s) ----
 * Order from low to high address = actual push order:
 * stub pushes intno(+dummy err), then ds/es/fs/gs, then pushal - and
 * pushal leaves EDI at the LOWEST address.  CPU pushes eip/cs/eflags
 * (+esp/ss only when ring3 -> ring0).
 */
typedef struct int_frame {
    uint32_t edi, esi, ebp, esp_k, ebx, edx, ecx, eax;  /* pushal */
    uint32_t gs, fs, es, ds;
    uint32_t intno, err;                                 /* stub pushes */
    uint32_t eip, cs, eflags;
    uint32_t esp, ss;      /* valid only when frame came from ring3 */
} int_frame_t;

/* ---- cpu.s exports ---- */
extern uint8_t gdt_ptr[6];
extern uint16_t idt_limit;

void     cpu_gdt_load(void);          /* lgdt + ltr */
void     cpu_idt_load(void);          /* lidt */
void     cpu_idle_halt(void);         /* sti; hlt (atomically wait for IRQ) */

uint32_t cpu_cr3_read(void);
uint32_t cpu_cr2_read(void);
void     cpu_cr3_write(uint32_t phys);
void     cpu_invlpg(uint32_t vaddr);
void     cpu_paging_enable(uint32_t kdir_phys);  /* PSE + PG on, cr3=kdir */

/* ring0 cooperative switch: switch_to(&prev->ksp, &next->ksp) */
void     cpu_switch_to(uint32_t *save_esp, uint32_t load_esp);

/* park the current task then enter a ring3 frame (see cpu.s) */
void     cpu_switch_park_and_resume(uint32_t *save_esp, uint32_t frame_esp,
                                    uint32_t cr3, uint32_t kstack_top);

/* resume a saved ISR-style frame (enter / re-enter ring3): sets cr3,
 * loads esp=frame and executes the stub epilogue (popal/iretd). */
void     cpu_frame_resume(uint32_t frame_esp, uint32_t cr3, uint32_t kstack_top);

/* ---- C handlers called from cpu.s ---- */
void isr_exception(int_frame_t *f);   /* 0..31 */
void isr_irq(int_frame_t *f);         /* 0x20..0x2F */
void isr_syscall(int_frame_t *f);     /* 0x80 */
uint32_t sched_timer_tick(int_frame_t *f); /* returns new esp (== f if no switch) */

/* ---- tss ---- */
typedef struct tss {
    uint16_t prev, pad0;
    uint32_t esp0, ss0, esp1, ss1, esp2, ss2;
    uint32_t cr3, eip, eflags, eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint16_t es, cs, ss, ds, fs, gs, ldt, pad1;
    uint16_t trap, iomap_base;
} __attribute__((packed)) tss_t;
extern tss_t tss;
void tss_set_kstack(uint32_t top);

#endif
