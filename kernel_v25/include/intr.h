#ifndef INTR_H
#define INTR_H

#include <stdint.h>

/* intr.c: GDT/IDT/PIC/PIT + tick counter (see cpu.h for frames/TSS) */

void     intr_init(void);            /* descriptors + PIC remap + PIT start */
void     intr_enable(void);          /* unmask timer+kbd IRQs and sti */
/* v2.21: PCI devices report their IRQ line in config 0x3C and can land on
 * any of 9..15.  Drivers call this after a successful probe; intr_enable()
 * deliberately leaves those lines masked. */
void     intr_unmask_irq(int irq);
void     intr_timer_init(uint32_t hz);
uint32_t uptime_ms(void);
void     exc_dump(struct int_frame *f);

extern volatile uint32_t timer_ticks;
extern uint32_t timer_hz_v;

#endif
