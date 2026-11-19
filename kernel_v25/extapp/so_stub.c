/*
 * so_stub.c -- freestanding runtime stubs for CSO1 shared libraries.
 *
 * Same __chkstk_ms no-op as ce_stub.c (MinGW emits a stack-probe call for
 * large frames, which is pointless on CatOS's pre-mapped kernel stack).
 * so_entry() merely gives GNU ld a defined -e symbol; the CSO1 loader never
 * jumps to it -- a library is entered through dlsym()'s resolved exports.
 */
void __chkstk_ms(void) {
    __asm__ volatile("" ::: "memory");
}

void so_entry(void) {
}
