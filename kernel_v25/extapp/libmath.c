/*
 * libmath.c -- first CSO1 shared object for CatOS (v2.47).
 *
 * Build:
 *   sh build_so.sh libmath.c bin 0x2700000 \
 *      add=lib_add,sub=lib_sub,mul=lib_mul,gcd=lib_gcd,version=lib_version,hello=lib_hello
 *
 * The library is linked at 0x2700000 and loaded there by dlopen().  It
 * receives the vsyscall table in __ce_api exactly like a normal CE app,
 * which is how lib_hello() can call api->write().
 */
#include "ce_abi.h"

catos_api_t *__ce_api __attribute__((section(".ce_api")));

int lib_add(int a, int b) { return a + b; }
int lib_sub(int a, int b) { return a - b; }
int lib_mul(int a, int b) { return a * b; }

int lib_gcd(int a, int b) {
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    while (b) { int t = a % b; a = b; b = t; }
    return a;
}

const char *lib_version(void) { return "libmath 1.0 (CSO1 shared object)"; }

void lib_hello(void) {
    if (__ce_api && __ce_api->write)
        __ce_api->write("[libmath] hello from a shared object\n");
}
