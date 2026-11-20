/*
 * dltest.c -- runtime dynamic-link demo (v2.47).
 *
 * Loads /bin/libmath.so with dlopen(), resolves exports by name with
 * dlsym(), calls them through plain C function pointers, then dlclose()s.
 * This proves that code not linked into the .ce image can be loaded and
 * executed at runtime through the append-only catos_api_t ABI.
 */
#include "ce_abi.h"

catos_api_t *__ce_api __attribute__((section(".ce_api"))) = (catos_api_t *)0;

typedef int        (*fn2i)(int, int);
typedef const char *(*fn0s)(void);
typedef void       (*fn0v)(void);

__attribute__((section(".text.ce_entry")))
int ce_main(void) {
    catos_api_t *api = CE_API();
    api->write("dltest: CSO1 dynamic-link demo\n");

    if (!api->dlopen || !api->dlsym || !api->dlclose) {
        api->write("dltest: kernel lacks the v2.47 dlopen ABI\n");
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

    if (!add || !sub || !mul || !gcd || !ver || !hello) {
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

    /* Unknown symbols and bad handles must fail safely (return NULL). */
    if (api->dlsym(h, "no_such_symbol") != 0)
        api->write("dltest: WARNING unknown symbol resolved!\n");
    if (api->dlsym(9999, "add") != 0)
        api->write("dltest: WARNING bad handle resolved!\n");

    api->write("dltest: dlclose -> ");
    api->print_int(api->dlclose(h));
    api->write(" (0=ok)\ndltest: PASS\n");
    return 0;
}
