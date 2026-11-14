#ifndef CATOS_DIAG_H
#define CATOS_DIAG_H

#include <stdint.h>

/* Last fatal fault seen, ring0 or ring3.  Queried by the shell's
 * `faultinfo` command; .bss-zeroed means "no fault since boot". */
typedef struct fault_record {
    int      valid;
    int      intno;     /* CPU vector */
    int      ring;      /* 0 or 3 */
    int      tid;       /* -1 when unknown */
    char     task[16];
    uint32_t err, eip, cr2, eflags;
    uint32_t ticks;     /* timer_ticks at the fault */
} fault_record_t;

extern fault_record_t g_fault_last;

/* Called from the exception dispatcher for every fatal fault. */
void fault_record(int intno, uint32_t err, uint32_t eip, uint32_t cr2,
                  uint32_t cs, uint32_t eflags);
void faultinfo_print(void);
void panic(const char *msg) __attribute__((noreturn));

#endif /* CATOS_DIAG_H */
