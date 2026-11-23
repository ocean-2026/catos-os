/* idt64.c - P0.4: IDT (exceptions + PIC/PIT IRQ + int 0x80), TSS, PIC */
#include "k64.h"

struct idt_entry {
    u16 off_lo;
    u16 sel;
    u8  ist;
    u8  type;
    u16 off_mid;
    u32 off_hi;
    u32 zero;
} __attribute__((packed));

static struct idt_entry idt[256];
static struct { u16 limit; u64 base; } __attribute__((packed)) idtr;

u64 tss64[16] __attribute__((aligned(16)));
static u64 ticks;

extern void isr0(void);  extern void isr1(void);  extern void isr2(void);
extern void isr3(void);  extern void isr4(void);  extern void isr5(void);
extern void isr6(void);  extern void isr7(void);  extern void isr8(void);
extern void isr9(void);  extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void);
extern void isr15(void); extern void isr16(void); extern void isr17(void);
extern void isr18(void); extern void isr19(void); extern void isr20(void);
extern void isr21(void); extern void isr22(void); extern void isr23(void);
extern void isr24(void); extern void isr25(void); extern void isr26(void);
extern void isr27(void); extern void isr28(void); extern void isr29(void);
extern void isr30(void); extern void isr31(void);
extern void int80_entry(void);

extern void irq0(void);  extern void irq1(void);  extern void irq2(void);
extern void irq3(void);  extern void irq4(void);  extern void irq5(void);
extern void irq6(void);  extern void irq7(void);  extern void irq8(void);
extern void irq9(void);  extern void irq10(void); extern void irq11(void);
extern void irq12(void); extern void irq13(void); extern void irq14(void);
extern void irq15(void);

extern u8 gdt64[];

static void gate(int n, void (*fn)(void), u8 dpl)
{
    u64 a = (u64)fn;
    idt[n].off_lo  = (u16)(a & 0xFFFF);
    idt[n].sel     = SEL_KCS64;
    idt[n].ist     = 0;
    idt[n].type    = (u8)(0x80 | (dpl << 5) | 0x0E);   /* P=1, DPL, 64-bit int gate */
    idt[n].off_mid = (u16)((a >> 16) & 0xFFFF);
    idt[n].off_hi  = (u32)(a >> 32);
    idt[n].zero    = 0;
}

static void outb(u16 port, u8 v) { __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(port)); }

void pic_init(void)
{
    outb(0x20, 0x11); outb(0xA0, 0x11);
    outb(0x21, 0x20); outb(0xA1, 0x28);
    outb(0x21, 0x04); outb(0xA1, 0x02);
    outb(0x21, 0x01); outb(0xA1, 0x01);
    outb(0x21, 0xFE); outb(0xA1, 0xFF);
}

void pit_init(u64 hz)
{
    u64 div = 1193182 / (hz ? hz : 1000);
    outb(0x43, 0x36);
    outb(0x40, (u8)(div & 0xFF));
    outb(0x40, (u8)((div >> 8) & 0xFF));
}

void idt64_init(void)
{
    void (*const exc[32])(void) = {
        isr0, isr1, isr2, isr3, isr4, isr5, isr6, isr7,
        isr8, isr9, isr10, isr11, isr12, isr13, isr14, isr15,
        isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23,
        isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31
    };
    void (*const irq[16])(void) = {
        irq0, irq1, irq2, irq3, irq4, irq5, irq6, irq7,
        irq8, irq9, irq10, irq11, irq12, irq13, irq14, irq15
    };

    for (int i = 0; i < 256; i++) {
        idt[i].off_lo = 0; idt[i].sel = 0; idt[i].ist = 0;
        idt[i].type = 0; idt[i].off_mid = 0; idt[i].off_hi = 0; idt[i].zero = 0;
    }
    for (int i = 0; i < 32; i++) gate(i, exc[i], 0);
    for (int i = 0; i < 16; i++) gate(32 + i, irq[i], 0);
    gate(0x80, int80_entry, 3);               /* DPL3 syscall gate */

    idtr.limit = (u16)(sizeof(idt) - 1);
    idtr.base  = (u64)&idt[0];
    __asm__ volatile ("lidt %0" : : "m"(idtr));

    /* TSS: RSP0 is patched by kmain (tss_set_rsp0); IOPM offset past the limit */
    for (int i = 0; i < 16; i++) tss64[i] = 0;
    *(u32 *)((u8 *)tss64 + 100) = 0x68 + 8;

    /* GDT slot 0x28: 64-bit TSS descriptor */
    u8 *g = gdt64 + 0x18;
    u64 base = (u64)tss64;
    u64 limit = 103;
    g[0]  = (u8)(limit & 0xFF);
    g[1]  = (u8)(limit >> 8);
    g[2]  = (u8)(base & 0xFF);
    g[3]  = (u8)((base >> 8) & 0xFF);
    g[4]  = (u8)((base >> 16) & 0xFF);
    g[5]  = 0x89;                             /* present, available 64-bit TSS */
    g[6]  = 0x00;
    g[7]  = (u8)((base >> 24) & 0xFF);
    g[8]  = (u8)(base >> 32);
    g[9]  = (u8)(base >> 40);
    g[10] = (u8)(base >> 48);
    g[11] = (u8)(base >> 56);
    g[12] = g[13] = g[14] = g[15] = 0;

    __asm__ volatile ("ltr %0" : : "ax"(SEL_TSS64));
}

void tss_set_rsp0(u64 rsp0)
{
    *(u64 *)((u8 *)tss64 + 4) = rsp0;
}

void exc_dispatch(u64 vec, u64 err, u64 rip)
{
    serial_puts("\n[k64] PANIC: exception vec=");
    serial_dec(vec);
    serial_puts(" err=");
    serial_hex(err);
    serial_puts(" rip=");
    serial_hex(rip);
    serial_puts("\n");
    for (;;) __asm__ volatile ("cli; hlt");
}

void irq_dispatch(u64 vec)
{
    if (vec == 32) ticks++;
    if (vec >= 40) outb(0xA0, 0x20);
    outb(0x20, 0x20);
}

u64 kuptime_ticks(void) { return ticks; }

