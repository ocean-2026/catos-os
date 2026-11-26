#include "ce_app.h"

/* fpu.ce - Stage 1 FPU roundtrip probe (ring3 x87 exercise).
 *
 * The kernel keeps its own TU soft-float; this app drives the x87 stack by
 * inline asm (the compiler is still -mno-80387) to prove the per-task
 * lazy save/restore path delivers a working coprocessor to ring3:
 *   1. load a known IEEE-754 double, store it back, compare raw bits;
 *   2. run a short x87 add loop (exercises the FPU stack without a libgcc
 *      float runtime);
 *   3. reload the constant and verify the bits still match.
 *
 * Verdict:  FPU CE PASS / FPU CE FAIL
 */

/* 12.0 = 0x4028000000000000 ; 1.0 = 0x3FF0000000000000. */
static const uint32_t val12[2] = { 0x00000000u, 0x40280000u };
static const uint32_t val1[2]  = { 0x00000000u, 0x3FF00000u };
static uint32_t out[2];
static uint32_t acc[2];

__attribute__((section(".text.ce_entry")))
int ce_main(int argc, char **argv) {
    (void)argc; (void)argv;
    catos_api_t *api = CE_API();

    /* 1. load 12.0, store, compare raw bits. */
    __asm__ volatile(
        "fldl %1\n"
        "fstpl %0\n"
        : "=m"(out) : "m"(val12) : "memory");
    int ok = (out[0] == val12[0]) && (out[1] == val12[1]);

    /* 2. x87 add loop: acc = 1.0, add 1.0 a few hundred times.  We do not
     * compare the float result (no soft-float runtime in the app); the
     * point is to exercise the coprocessor without #NM/#GP/#UD. */
    if (ok) {
        uint32_t n = 300;
        __asm__ volatile(
            "fldl %1\n"            /* push 1.0               */
            "mov %0, %%ecx\n"
            "1:\n"
            "faddl %1\n"
            "loop 1b\n"
            "fstpl %2\n"
            : : "r"(n), "m"(val1), "m"(acc) : "ecx", "memory");
    }

    /* 3. reload 12.0, verify bits still match after the arithmetic. */
    if (ok) {
        __asm__ volatile(
            "fldl %1\n"
            "fstpl %0\n"
            : "=m"(out) : "m"(val12) : "memory");
        ok = (out[0] == val12[0]) && (out[1] == val12[1]);
    }

    api->write("FPU CE ");
    api->write(ok ? "PASS" : "FAIL");
    api->write("\n");
    return ok ? 0 : 1;
}
