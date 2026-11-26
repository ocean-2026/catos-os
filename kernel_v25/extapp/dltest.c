/*
 * dltest.c -- runtime dynamic-link demo (stage 2C block ②).
 *
 * Loads /bin/libmath.so with dlopen(), resolves exports by name with
 * dlsym(), calls them through function pointers, then dlclose()s.  Beyond
 * the basic arithmetic exports it also exercises:
 *   - `combo`: internally calls lib_add through the PLT and reads the
 *     exported `seed` variable through the GOT;
 *   - `dispatch`: calls through a relocated rodata function-pointer table;
 *   - `seed`: an exported data symbol resolved to its storage address.
 */
#include "ce_abi.h"

catos_api_t *__ce_api __attribute__((section(".ce_api"))) = (catos_api_t *)0;

typedef int        (*fn2i)(int, int);
typedef int        (*fn1i)(int);
typedef int        (*fn3i)(int, int, int);
typedef const char *(*fn0s)(void);
typedef void       (*fn0v)(void);

__attribute__((section(".text.ce_entry")))
int ce_main(void) {
    catos_api_t *api = CE_API();
    api->write("dltest: CSO2 dynamic-link demo\n");

    if (!api->dlopen || !api->dlsym || !api->dlclose) {
        api->write("dltest: kernel lacks the dlopen ABI\n");
        return 1;
    }

    int h = api->dlopen("/bin/libmath.so");
    if (h <= 0) {
        api->write("dltest: dlopen failed (errno ");
        api->print_int(h);
        api->write(")\n");
        return 2;
    }
    api->write("dltest: loaded libmath.so, handle=");
    api->print_int(h);
    api->write("\n");

    fn2i add = (fn2i)api->dlsym(h, "add");
    fn2i sub = (fn2i)api->dlsym(h, "sub");
    fn2i mul = (fn2i)api->dlsym(h, "mul");
    fn2i gcd = (fn2i)api->dlsym(h, "gcd");
    fn0s ver = (fn0s)api->dlsym(h, "version");
    fn0v hello = (fn0v)api->dlsym(h, "hello");
    fn1i combo = (fn1i)api->dlsym(h, "combo");
    fn3i dispatch = (fn3i)api->dlsym(h, "dispatch");
    int *seedp = (int *)api->dlsym(h, "seed");

    if (!add || !sub || !mul || !gcd || !ver || !hello ||
        !combo || !dispatch || !seedp) {
        api->write("dltest: dlsym returned NULL for an export\n");
        api->dlclose(h);
        return 3;
    }

    api->write(ver());
    api->write("\n");
    hello();

    api->write("7+5="); api->print_int(add(7, 5));
    api->write(" 7-5="); api->print_int(sub(7, 5));
    api->write(" 7*5="); api->print_int(mul(7, 5));
    api->write(" gcd(84,36)="); api->print_int(gcd(84, 36));
    api->write("\n");

    /* Exported variable read through its resolved storage address. */
    api->write("seed="); api->print_int(*seedp);

    /* combo() PLT-calls lib_add(seed) internally; expect 5 + seed. */
    int c = combo(5);
    api->write(" combo(5)="); api->print_int(c);

    /* dispatch() walks the relocated rodata pointer table: 0=add 1=sub 2=mul */
    api->write(" dispatch(0,7,5)="); api->print_int(dispatch(0, 7, 5));
    api->write(" dispatch(1,7,5)="); api->print_int(dispatch(1, 7, 5));
    api->write(" dispatch(2,7,5)="); api->print_int(dispatch(2, 7, 5));
    api->write("\n");

    int ok = (c == 5 + *seedp) &&
             dispatch(0, 7, 5) == 12 && dispatch(1, 7, 5) == 2 &&
             dispatch(2, 7, 5) == 35;

    /* Unknown symbols and bad handles must fail safely (return NULL). */
    if (api->dlsym(h, "no_such_symbol") != 0)
        api->write("dltest: WARNING unknown symbol resolved!\n");
    if (api->dlsym(9999, "add") != 0)
        api->write("dltest: WARNING bad handle resolved!\n");

    api->write("dltest: dlclose -> ");
    api->print_int(api->dlclose(h));
    api->write(" (0=ok)\n");
    if (ok) {
        api->write("dltest: PASS\n");
        return 0;
    }
    api->write("dltest: FAIL\n");
    return 4;
}
