/* fpu.c - per-task FPU (x87 / SSE) lazy context switching.
 *
 * See fpu.h for the model.  The kernel TU is still compiled soft-float
 * (-mno-80387 -mno-sse); the only coprocessor instructions here are the
 * explicit save/restore/init inline asm, chosen by CPUID capability.
 */

#include "fpu.h"
#include "task.h"
#include "cpu.h"
#include "serial_log.h"

/* ---- low-level control registers ------------------------------------- */

static uint32_t cr0_read(void) {
    uint32_t v;
    __asm__ volatile("mov %%cr0, %0" : "=r"(v));
    return v;
}
static void cr0_write(uint32_t v) {
    __asm__ volatile("mov %0, %%cr0" :: "r"(v));
}
static uint32_t cr4_read(void) {
    uint32_t v;
    __asm__ volatile("mov %%cr4, %0" : "=r"(v));
    return v;
}
static void cr4_write(uint32_t v) {
    __asm__ volatile("mov %0, %%cr4" :: "r"(v));
}

/* ---- detected capability --------------------------------------------- */

static uint32_t g_caps;
static task_t  *g_fpu_owner;          /* task whose state is live in the
                                         * coprocessor right now, or NULL */

uint32_t fpu_caps(void) { return g_caps; }

static void cpuid1(uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx) {
    __asm__ volatile("cpuid"
                     : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                     : "a"(1));
}

/* ---- save / restore (instruction set chosen by capability) ---------- */

static void fpu_save(uint8_t *buf) {
    if (g_caps & FPU_CAP_FXSR) {
        __asm__ volatile("fxsave %0" : "=m"(*(volatile uint8_t (*)[512])buf)
                         : : "memory");
    } else {
        __asm__ volatile("fnsave %0" : "=m"(*(volatile uint8_t (*)[512])buf)
                         : : "memory");
    }
}

static void fpu_restore(uint8_t *buf) {
    if (g_caps & FPU_CAP_FXSR) {
        __asm__ volatile("fxrstor %0" : : "m"(*(volatile uint8_t (*)[512])buf)
                         : "memory");
    } else {
        __asm__ volatile("frstor %0" : : "m"(*(volatile uint8_t (*)[512])buf)
                         : "memory");
    }
}

/* ---- public lifecycle ------------------------------------------------- */

void fpu_init(void) {
    uint32_t a, b, c, d;
    g_caps = 0;
    g_fpu_owner = 0;

    cpuid1(&a, &b, &c, &d);
    if (d & (1u << 0))  g_caps |= FPU_CAP_X87;
    if (d & (1u << 24)) g_caps |= FPU_CAP_FXSR;
    if (d & (1u << 25)) g_caps |= FPU_CAP_SSE;
    if (d & (1u << 26)) g_caps |= FPU_CAP_SSE2;

    /* CR4: OSFXSR (bit9) / OSXMMEXCPT (bit10) only when the CPU advertises
     * the relevant SSE support.  Do not assume SSE2 on qemu32. */
    uint32_t cr4 = cr4_read();
    if (g_caps & FPU_CAP_FXSR)  cr4 |= 1u << 9;
    if (g_caps & FPU_CAP_SSE)   cr4 |= 1u << 10;
    cr4_write(cr4);

    /* CR0: clear EM (emulation) and TS while we initialise; the kernel
     * itself never executes coprocessor math, only the explicit init below. */
    uint32_t cr0 = cr0_read();
    cr0 &= ~(1u << 2);              /* EM  = 0: FPU present            */
    cr0 &= ~(1u << 3);              /* TS  = 0: allow init insn         */
    cr0_write(cr0);

    if (g_caps & FPU_CAP_X87) {
        __asm__ volatile("fninit");
    }

    /* Raise TS: lazy switch.  The next coprocessor use traps #NM. */
    cr0 = cr0_read();
    cr0 |= 1u << 3;
    cr0_write(cr0);

    ser_write("[fpu] caps="); ser_hex("", g_caps);
    ser_write(" cr4="); ser_hex("", cr4_read());
    ser_write(" cr0="); ser_hex("", cr0_read()); ser_write("\n");
}

void fpu_on_switch(void) {
    /* Lazy: mark the coprocessor "dirty" so the incoming task traps #NM
     * on first use.  The live state is left in the registers; the prior
     * owner's save area is only written on the next #NM. */
    uint32_t cr0 = cr0_read();
    cr0 |= 1u << 3;                 /* TS */
    cr0_write(cr0);
}

void fpu_nm_trap(struct task *cur) {
    if (!cur) return;
    /* Clear TS before any save/restore insn, otherwise the FPU instruction
     * would re-trap #NM recursively. */
    __asm__ volatile("clts");

    task_t *owner = g_fpu_owner;
    if (owner && owner != cur) {
        fpu_save(owner->fpu_save);
        owner->fpu_used = 1;
    }

    if (cur->fpu_used) {
        fpu_restore(cur->fpu_save);
    } else if (g_caps & FPU_CAP_X87) {
        __asm__ volatile("fninit");
    }
    cur->fpu_used = 1;
    g_fpu_owner = (task_t *)cur;
}

void fpu_task_cleanup(struct task *t) {
    if (!t) return;
    if (g_fpu_owner == t) g_fpu_owner = 0;
    t->fpu_used = 0;
}

/* ---- shell diagnostic -------------------------------------------------- */

/* 12.0 as an IEEE-754 double: lo=0, hi=0x40280000. */
static const uint32_t diag_val[2]      = { 0x00000000u, 0x40280000u };
/* 7.0: lo=0, hi=0x401C0000. */
static const uint32_t diag_clobber[2]  = { 0x00000000u, 0x401C0000u };
static uint8_t  diag_save[512] __attribute__((aligned(16)));
static uint32_t diag_out2[2];

static int sap(char *buf, int max, int n, const char *s) {
    while (*s && n < max - 1) buf[n++] = *s++;
    buf[n] = 0;
    return n;
}
static int sap_hex(char *buf, int max, int n, uint32_t v) {
    static const char *h = "0123456789abcdef";
    buf[n++] = '0'; buf[n++] = 'x';
    for (int i = 0; i < 8; i++) {
        if (n < max - 1) buf[n++] = h[(v >> (28 - i * 4)) & 0xF];
    }
    buf[n] = 0;
    return n;
}

int fpu_diag(char *buf, int max) {
    if (!buf || max <= 0) return -1;
    int n = 0;
    uint32_t caps = g_caps;

    n = sap(buf, max, n, "== fpu diagnostic ==\n");
    n = sap(buf, max, n, "caps : ");
    if (caps & FPU_CAP_X87)  n = sap(buf, max, n, "x87 ");
    if (caps & FPU_CAP_FXSR) n = sap(buf, max, n, "fxsr ");
    if (caps & FPU_CAP_SSE)   n = sap(buf, max, n, "sse ");
    if (caps & FPU_CAP_SSE2)  n = sap(buf, max, n, "sse2");
    if (caps == 0)            n = sap(buf, max, n, "none");
    n = sap(buf, max, n, "\n");

    uint32_t cr0 = cr0_read(), cr4 = cr4_read();
    n = sap(buf, max, n, "cr0  : "); n = sap_hex(buf, max, n, cr0);
    n = sap(buf, max, n, "  TS=");  n = sap(buf, max, n, (cr0 >> 3) & 1 ? "1" : "0");
    n = sap(buf, max, n, " EM=");   n = sap(buf, max, n, (cr0 >> 2) & 1 ? "1" : "0");
    n = sap(buf, max, n, "\n");
    n = sap(buf, max, n, "cr4  : "); n = sap_hex(buf, max, n, cr4);
    n = sap(buf, max, n, "  OSFXSR=");  n = sap(buf, max, n, (cr4 >> 9) & 1 ? "1" : "0");
    n = sap(buf, max, n, " OSXMMEXCPT="); n = sap(buf, max, n, (cr4 >> 10) & 1 ? "1" : "0");
    n = sap(buf, max, n, "\n");

    n = sap(buf, max, n, "owner: tid=");
    n = sap_hex(buf, max, n, g_fpu_owner ? (uint32_t)g_fpu_owner->tid : 0xFFFFFFFFu);
    n = sap(buf, max, n, "\n");

    /* In-kernel save/restore roundtrip: load 12.0, save, clobber with
     * 7.0 + fninit, restore, read back; raw bits must match diag_val.
     * The shell runs ring0; clear TS for the explicit FPU use here, then
     * restore TS so the lazy model still traps the next ring3 task. */
    if (!(caps & FPU_CAP_X87)) {
        n = sap(buf, max, n, "roundtrip: SKIP (no x87)\n");
        return n;
    }

    __asm__ volatile("clts");

    /* Push 12.0 and keep it on the x87 stack through the save; clobber
     * with fninit + 7.0, restore, then pop and compare raw bits. */
    __asm__ volatile(
        "fldl %0\n"
        : : "m"(diag_val) : "memory");

    fpu_save(diag_save);

    __asm__ volatile("fninit");
    __asm__ volatile("fldl %0" : : "m"(diag_clobber) : "memory");

    fpu_restore(diag_save);

    __asm__ volatile("fstpl %0" : "=m"(diag_out2) : : "memory");

    int ok = (diag_out2[0] == diag_val[0]) && (diag_out2[1] == diag_val[1]);
    n = sap(buf, max, n, "roundtrip: ");
    n = sap(buf, max, n, ok ? "PASS" : "FAIL");
    n = sap(buf, max, n, "  expect="); n = sap_hex(buf, max, n, diag_val[1]);
    n = sap(buf, max, n, ":"); n = sap_hex(buf, max, n, diag_val[0]);
    n = sap(buf, max, n, " out2="); n = sap_hex(buf, max, n, diag_out2[1]);
    n = sap(buf, max, n, ":"); n = sap_hex(buf, max, n, diag_out2[0]);
    n = sap(buf, max, n, "\n");

    fpu_on_switch();            /* re-raise TS for the next ring3 FPU use */
    return n;
}
