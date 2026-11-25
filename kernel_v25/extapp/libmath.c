/*
 * libmath.c -- CSO2 shared object for CatOS (stage 2C block ②).
 *
 * Build:
 *   sh build_so.sh libmath.c bin 0x2700000 \
 *      add=lib_add,sub=lib_sub,mul=lib_mul,gcd=lib_gcd,version=lib_version,\
 *      hello=lib_hello,combo=lib_combo,seed=lib_seed,dispatch=lib_dispatch
 *
 * PIC -shared ELF wrapped into a CSO2 image.  Exercises the real reloc paths:
 *   - `lib_seed` is an exported data symbol referenced through the GOT
 *     (R_386_GLOB_DAT, 6);
 *   - `lib_combo()` calls the exported `lib_add()` through the PLT
 *     (R_386_JMP_SLOT, 7);
 *   - `g_fns[]` holds absolute function addresses in rodata, needing
 *     R_386_RELATIVE(8) base-relative fixups.
 */
#include "ce_abi.h"

catos_api_t *__ce_api __attribute__((section(".ce_api")));

/* Exported variable: its address is taken through the GOT (GLOB_DAT). */
int lib_seed = 7;

int lib_add(int a, int b) { return a + b; }
int lib_sub(int a, int b) { return a - b; }
int lib_mul(int a, int b) { return a * b; }

int lib_gcd(int a, int b) {
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    while (b) { int t = a % b; a = b; b = t; }
    return a;
}

/* Calls the exported lib_add() through the PLT (JMP_SLOT) and reads the
 * exported lib_seed through the GOT (GLOB_DAT).  Proves the PLT/GOT slots
 * are patched at load time rather than the call being a direct reference. */
int lib_combo(int x) { return lib_add(x, lib_seed); }

static int (* const g_fns[2])(int, int) = { lib_add, lib_sub };

/* Read-only table of function pointers; slots hold absolute link-time
 * addresses and need R_386_RELATIVE(8) base-relative fixups.  The local
 * helper is not exported, so its pointer slot uses RELATIVE rather than a
 * symbolic GLOB_DAT/32 reloc. */
static int local_mul(int a, int b) { return a * b; }
static int (* const g_local[1])(int, int) = { local_mul };

int lib_dispatch(int i, int a, int b) {
    if (i == 2) return g_local[0](a, b);
    return g_fns[i](a, b);
}

const char *lib_version(void) { return "libmath 2.0 (CSO2 shared object)"; }

void lib_hello(void) {
    if (__ce_api && __ce_api->write)
        __ce_api->write("[libmath] hello from a shared object\n");
}
