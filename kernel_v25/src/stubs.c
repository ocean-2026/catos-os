/* stubs.c - minimal runtime stubs for freestanding i386 builds */
/* These replace compiler-inserted runtime calls that we don't need. */

/* Stack check stub: GCC may insert __chkstk_ms for large stack frames.
 * Our kernels don't use dynamic stack probing, so this is a no-op. */
void __chkstk_ms(void) { }

/* Similarly for any other missing runtime symbols */
void __main(void) { }
