/*
 * chkstk.c -- freestanding runtime stub for the kernel.
 *
 * Modern MinGW gcc (16.x) emits __chkstk_ms for stack frames larger than
 * one page (fs.c has one).  The kernel runs on a known, already-mapped
 * stack, so probing is pointless; the probe must simply be a no-op.
 * Mirrors extapp/ce_stub.c, which does the same for CE programs.
 */
void __chkstk_ms(void) {
    __asm__ volatile("" ::: "memory");
}
