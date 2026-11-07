/* intr.c - GDT / IDT / PIC / PIT: the interrupt pillar of CatOS.
 *
 * intr_init() builds the GDT (ring0 + ring3 + TSS), installs all gates,
 * remaps the PIC, starts the 100 Hz PIT and leaves interrupts masked off
 * until intr_enable() (called once the scheduler is up).
 *
 * Exceptions dump the frame and halt on ring0 faults; in a ring3 task
 * they kill the task instead of the system (isolation pillar).
 */

#include "cpu.h"
#include "io.h"
#include "vga.h"
#include "keyboard.h"
#include "task.h"

/* ---- GDT -------------------------------------------------------------- */

static uint64_t gdt[6];

static void gdt_set(int i, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    gdt[i] =
        ((uint64_t)(limit & 0xFFFF)) |
        ((uint64_t)(base & 0xFFFFFF) << 16) |
        ((uint64_t)access << 40) |
        ((uint64_t)((limit >> 16) & 0x0F) << 48) |
        ((uint64_t)(gran & 0x0F) << 52) |
        ((uint64_t)((base >> 24) & 0xFF) << 56);
}

/* descriptors in cpu.s: { uint16_t limit; uint32_t base; } */
typedef struct { uint16_t limit; uint32_t base; } __attribute__((packed)) desc_ptr_t;
extern desc_ptr_t gdt_desc;   /* = _gdt_desc in cpu.s */
extern desc_ptr_t idt_desc;

/* ---- IDT -------------------------------------------------------------- */

typedef struct {
    uint16_t off_lo, sel;
    uint8_t  zero, flags;      /* 0x8E = ring0 gate, 0xEE = ring3 gate */
    uint16_t off_hi;
} __attribute__((packed)) idt_entry_t;

static idt_entry_t idt[256];

/* cpu.s exports a table of 49 stub addresses: 0..31 exceptions,
 * 32..47 IRQs, 48 = int 0x80 system call */
extern void (*isr_stub_table[49])(void);

static void idt_set(int v, uint32_t handler, uint8_t flags) {
    idt[v].off_lo = (uint16_t)(handler & 0xFFFF);
    idt[v].sel    = 0x08;
    idt[v].zero   = 0;
    idt[v].flags  = flags;
    idt[v].off_hi = (uint16_t)(handler >> 16);
}

/* ---- PIC / PIT -------------------------------------------------------- */

static void pic_remap(void) {
    outb(0x20, 0x11); outb(0xA0, 0x11);      /* ICW1: init, cascade */
    outb(0x21, 0x20); outb(0xA1, 0x28);      /* ICW2: vectors 0x20 / 0x28 */
    outb(0x21, 0x04); outb(0xA1, 0x02);      /* ICW3 */
    outb(0x21, 0x01); outb(0xA1, 0x01);      /* ICW4: 8086 mode */
    /* keep only cascade(2) enabled for now; timer(0)/kbd(1) are unmasked
     * in intr_enable() once the scheduler is ready */
    outb(0x21, (uint8_t)~(1 << 2));
    outb(0xA1, 0xFF);
}

void intr_timer_init(uint32_t hz) {
    uint32_t div = 1193182u / hz;
    outb(0x43, 0x36);                        /* ch0, lo/hi, mode 3 */
    outb(0x40, (uint8_t)(div & 0xFF));
    outb(0x40, (uint8_t)((div >> 8) & 0xFF));
}

void intr_enable(void) {
    outb(0x21, (uint8_t)~(1 << 0 | 1 << 1 | 1 << 2));  /* timer + kbd + cascade */
    __asm__ volatile ("sti");
}

/* ---- ticks ------------------------------------------------------------ */

volatile uint32_t timer_ticks;
uint32_t timer_hz_v = 100;

uint32_t uptime_ms(void) { return timer_ticks * (1000u / timer_hz_v); }

void tss_set_kstack(uint32_t top) { tss.esp0 = top; }

/* ---- exception dump ---------------------------------------------------- */

static const char *exc_name[32] = {
    "divide by zero", "debug", "nmi", "breakpoint", "overflow", "bound range",
    "invalid opcode", "device n/a", "double fault", "coproc seg", "invalid tss",
    "segment not present", "stack fault", "general protection fault", "page fault",
    "reserved", "x87 fp", "alignment", "machine check", "simd fp",
    "virt", "cp", "reserved", "reserved", "reserved", "reserved", "reserved",
    "reserved", "reserved", "reserved", "reserved", "reserved",
};

static void whex(uint32_t v) {
    static const char *h = "0123456789abcdef";
    char b[11];
    b[0] = '0'; b[1] = 'x';
    for (int i = 0; i < 8; i++) b[2 + i] = h[(v >> (28 - i * 4)) & 0xF];
    b[10] = 0;
    vga_write(b);
}

void exc_dump(int_frame_t *f) {
    vga_write("\n*** exception: ");
    vga_write(exc_name[f->intno < 32 ? f->intno : 31]);
    vga_write(" ***\n");
    vga_write("int=");   whex(f->intno);
    vga_write(" err=");  whex(f->err);
    vga_write(" eip=");  whex(f->eip);
    vga_write(" cr2=");  whex(cpu_cr2_read());
    vga_write(" cs=");   whex(f->cs);
    vga_write(" efl=");  whex(f->eflags);
    vga_write("\neax="); whex(f->eax);
    vga_write(" ebx=");  whex(f->ebx);
    vga_write(" ecx=");  whex(f->ecx);
    vga_write(" edx=");  whex(f->edx);
    vga_write(" esi=");  whex(f->esi);
    vga_write(" edi=");  whex(f->edi);
    vga_write(" esp=");  whex(f->esp_k);
    vga_write("\nstack:");
    for (int i = 0; i < 12; i++) { vga_write(" "); whex(((uint32_t *)f->esp_k)[i]); }
    vga_write("\n");
}

void isr_exception(int_frame_t *f) {
    if ((f->cs & 3) == 3) {
        /* ring3 fault:
         *   - Page fault (INT 0x0E) may be recoverable via lazy VMA mapping.
         *     Otherwise kill the offending task.
         *   - All other faults kill the task immediately. */
        if (f->intno == 14) {
            if (task_page_fault(f) == 0) return;  /* fault handled, retry */
        }
        task_die_fault(f);
    }
    /* ring0 kernel fault: dump and halt (no scheduler games in a broken kernel) */
    __asm__ volatile ("cli");
    exc_dump(f);
    vga_write("ring0 kernel fault - system halted\n");
    for (;;) __asm__ volatile ("hlt");
}

/* ---- IRQ dispatch ------------------------------------------------------ */

void isr_irq(int_frame_t *f) {
    int irq = f->intno - 0x20;
    if (irq == 8) {
        uint8_t isr2 = inb(0xA0);            /* spurious IRQ15 check */
        if (!(isr2 & 0x80)) { outb(0x20, 0x20); return; }
    }
    if (irq == 7) {
        uint8_t isr1 = inb(0x20);            /* spurious IRQ7 check */
        if (!(isr1 & 0x80)) return;
    }
    switch (irq) {
    case 0:
        timer_ticks++;
        task_tick_wake();                    /* wake sleepers, set need_resched */
        break;
    case 1:
        kb_irq();                            /* PS/2 scancodes -> ring buffer */
        break;
    default:
        break;
    }
    if (irq >= 8) outb(0xA0, 0x20);          /* slave EOI */
    outb(0x20, 0x20);                        /* master EOI */
}

/* ---- init --------------------------------------------------------------- */

void intr_init(void) {
    /* GDT: null, kcode, kdata, ucode, udata, tss */
    gdt_set(0, 0, 0, 0, 0);
    gdt_set(1, 0, 0xFFFFF, 0x9A, 0xCF);
    gdt_set(2, 0, 0xFFFFF, 0x92, 0xCF);
    gdt_set(3, 0, 0xFFFFF, 0xFA, 0xCF);      /* ring3 code */
    gdt_set(4, 0, 0xFFFFF, 0xF2, 0xCF);      /* ring3 data */
    gdt_set(5, (uint32_t)&tss, sizeof(tss_t) - 1, 0x89, 0x00);

    tss.ss0 = 0x10;
    tss.iomap_base = sizeof(tss_t);

    gdt_desc.limit = sizeof(gdt) - 1;
    gdt_desc.base  = (uint32_t)gdt;
    cpu_gdt_load();

    /* IDT: exceptions + IRQs + int 0x80 (DPL3 so ring3 can call) */
    for (int i = 0; i < 48; i++)
        idt_set(i, (uint32_t)isr_stub_table[i], 0x8E);
    idt_set(0x80, (uint32_t)isr_stub_table[48], 0xEE);

    idt_desc.limit = sizeof(idt) - 1;
    idt_desc.base  = (uint32_t)idt;
    pic_remap();
    intr_timer_init(100);
    cpu_idt_load();
}
