/* diag.c -- last-fault record + kernel panic hook.
 *
 * Before this, a ring0 fault printed a register dump and halted, and a
 * ring3 task death printed a single line; nothing was queryable after the
 * fact.  The exception dispatcher now records every fatal fault, so
 * `faultinfo` can show what happened -- including the page-fault
 * error-code bits -- even after the offending task is long gone.
 */
#include "diag.h"
#include "vga.h"
#include "intr.h"     /* timer_ticks */
#include "task.h"     /* task_current, task_t */

fault_record_t g_fault_last;

static const char *dexc_name(int n) {
    static const char *nm[32] = {
        "div0", "debug", "nmi", "breakpoint", "overflow", "bounds",
        "invalid-op", "no-fpu", "double-fault", "seg-overflow", "invalid-tss",
        "seg-not-present", "stack-fault", "gpf", "page-fault", "reserved",
        "x87", "alignment", "machine-check", "simd-fp", "virt", "cp",
        "reserved", "reserved", "reserved", "reserved", "reserved",
        "reserved", "reserved", "reserved", "security", "reserved",
    };
    return nm[(n >= 0 && n < 32) ? n : 31];
}

static void dhex(uint32_t v) {
    static const char *h = "0123456789abcdef";
    char b[11];
    b[0] = '0'; b[1] = 'x';
    for (int i = 0; i < 8; i++) b[2 + i] = h[(v >> (28 - i * 4)) & 0xF];
    b[10] = 0;
    vga_write(b);
}

void fault_record(int intno, uint32_t err, uint32_t eip, uint32_t cr2,
                  uint32_t cs, uint32_t eflags) {
    task_t *t = task_current();
    int i;
    g_fault_last.valid = 1;
    g_fault_last.intno = intno;
    g_fault_last.ring = (int)(cs & 3);
    g_fault_last.tid = t ? (int)t->tid : -1;
    g_fault_last.task[0] = 0;
    if (t) {
        for (i = 0; i < 15 && t->name[i]; i++)
            g_fault_last.task[i] = t->name[i];
        g_fault_last.task[i] = 0;
    }
    g_fault_last.err = err;
    g_fault_last.eip = eip;
    g_fault_last.cr2 = cr2;
    g_fault_last.eflags = eflags;
    g_fault_last.ticks = timer_ticks;
}

void faultinfo_print(void) {
    fault_record_t *r = &g_fault_last;
    if (!r->valid) { vga_write("no fault recorded since boot\n"); return; }
    vga_write("last fault: ");
    vga_write(dexc_name(r->intno));
    vga_write(" ring"); dhex((uint32_t)r->ring);
    vga_write(" task "); vga_write(r->task[0] ? r->task : "?");
    vga_write(" tid"); dhex((uint32_t)r->tid);
    vga_write("\neip="); dhex(r->eip);
    vga_write(" cr2="); dhex(r->cr2);
    vga_write(" err="); dhex(r->err);
    vga_write(" efl="); dhex(r->eflags);
    vga_write(" at tick "); dhex(r->ticks);
    vga_write("\n");
    if (r->intno == 14) {
        vga_write("pf bits: ");
        vga_write((r->err & 1) ? "present " : "not-present ");
        vga_write((r->err & 2) ? "write " : "read ");
        vga_write((r->err & 4) ? "user " : "supervisor ");
        if (r->err & 8) vga_write("rsvd ");
        if (r->err & 16) vga_write("instr-fetch ");
        vga_write("\n");
    }
}

void panic(const char *msg) {
    vga_write("\n*** KERNEL PANIC: ");
    vga_write(msg ? msg : "(no message)");
    vga_write(" ***\n");
    __asm__ volatile ("cli");
    for (;;) __asm__ volatile ("hlt");
}
