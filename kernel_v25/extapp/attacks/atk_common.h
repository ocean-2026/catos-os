/* atk_common.h -- shared helpers for the red-team crash attack CEs.
 *
 * Every attack program:
 *   - prints a STEP marker before each dangerous call, so that when the
 *     guest dies the last visible STEP identifies the killer call;
 *   - is self-terminating (bounded loops, prints ATK-DONE at the end);
 *   - never touches libc: only the CE ABI (65 slots, v2.14).
 *
 * Freestanding build: see extapp/build_ce.sh (gcc -m32 -ffreestanding ...).
 */
#ifndef ATK_COMMON_H
#define ATK_COMMON_H

#include "ce_abi.h"

static catos_api_t *A(void) { return CE_API(); }

static void w(const char *s) { A()->write(s); }
static void wd(int v) { A()->print_int(v); }
static void wh(uint32_t v) { A()->print_hex(v); }
static void nl(void) { A()->write("\n"); }

/* m("name", value) -> "name=value " */
static void m(const char *k, int v) { w(k); w("="); wd(v); w(" "); }
static void mh(const char *k, uint32_t v) { w(k); w("="); wh(v); w(" "); }
static void step(int n) { w("\nSTEP "); wd(n); w(": "); }
static void mark(const char *s) { w("-- "); w(s); w("\n"); }

/* Poison values from the red-team brief section 5: NULL, low page, VRAM,
 * kernel load area, the task's own image/vsyscall/heap/stack, the
 * extension arena, and the top-of-address-space edge cases. */
static const uint32_t POISON[] = {
    0x00000000u, 0x00000FFFu, 0x00001000u, 0x000B8000u, /* VRAM text      */
    0x000FFFFFu, 0x00100000u, 0x00148000u,             /* kernel image    */
    0x01FFFFFFu, 0x02000000u, 0x02080000u,             /* image/vsyscall  */
    0x02100000u, 0x021FFFFFu, 0x02200000u,             /* heap / stack    */
    0x023FF000u, 0x02400000u, 0x027FF000u,             /* ext arena edges */
    0x7FFFFFFFu, 0x80000000u, 0xFFFFF000u, 0xFFFFFFFCu, 0xFFFFFFFFu,
};
#define NPOISON ((int)(sizeof(POISON) / sizeof(POISON[0])))

/* Raw int 0x80 with full register control (bypasses the vsyscall thunk,
 * so illegal syscall numbers really reach the dispatcher). */
static int sc5(uint32_t nr, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4) {
    int r;
    __asm__ volatile ("int $0x80"
                      : "=a"(r)
                      : "a"(nr), "b"(a1), "c"(a2), "d"(a3), "S"(a4)
                      : "memory");
    return r;
}

static int sc0(uint32_t nr) { return sc5(nr, 0, 0, 0, 0); }

/* Raw "int $N" for vectors the kernel never expected from ring3. */
static void rawint(int vec) {
    switch (vec) {
    case 3:  __asm__ volatile ("int $3"  ::: "memory"); break;
    case 4:  __asm__ volatile ("int $4"  ::: "memory"); break;
    case 13: __asm__ volatile ("int $13" ::: "memory"); break;
    case 14: __asm__ volatile ("int $14" ::: "memory"); break;
    case 32: __asm__ volatile ("int $32" ::: "memory"); break;
    case 33: __asm__ volatile ("int $33" ::: "memory"); break;
    case 48: __asm__ volatile ("int $48" ::: "memory"); break;
    default: __asm__ volatile ("int $0x41" ::: "memory"); break; /* unset */
    }
}

#endif /* ATK_COMMON_H */
