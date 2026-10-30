#ifndef INTR_H
#define INTR_H

#include <stdint.h>

/* intr.c: GDT/IDT/PIC/PIT + tick counter (see cpu.h for frames/TSS) */

void     intr_init(void);            /* descriptors + PIC remap + PIT start */
void     intr_enable(void);          /* unmask timer+kbd IRQs and sti */
void     intr_timer_init(uint32_t hz);
uint32_t uptime_ms(void);
void     exc_dump(struct int_frame *f);

extern volatile uint32_t timer_ticks;
extern uint32_t timer_hz_v;

#endif
