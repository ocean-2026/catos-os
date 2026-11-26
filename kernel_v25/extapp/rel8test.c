/* rel8test.c -- 2C block ③ synthetic R_386_RELATIVE(8) probe app.
 *
 * Loads /bin/rel8.so (whose rela table carries one hand-crafted RELATIVE
 * entry), calls its exported val(), and checks the result.  The type-8
 * branch execution is evidenced by the serial line
 *   reloc: type=8 slot=0x... base=0x...
 * captured in gate-dynlink2-relative.log.
 *
 * Build: sh extapp/build_ce.sh rel8test.c
 */
#include "ce_abi.h"

catos_api_t *__ce_api __attribute__((section(".ce_api")));
#define CE_API() (__ce_api)

typedef int (*fn0i)(void);

__attribute__((section(".text.ce_entry")))
int ce_main(void) {
    catos_api_t *a = CE_API();
    a->write("rel8test: synthetic RELATIVE probe\n");

    int h = a->dlopen("/bin/rel8.so");
    if (h <= 0) {
        a->write("rel8test: dlopen failed (");
        a->print_int(h); a->write(")\n");
        return 2;
    }

    fn0i val = (fn0i)a->dlsym(h, "val");
    if (!val) {
        a->write("rel8test: dlsym NULL\n");
        a->dlclose(h);
        return 3;
    }

    int v = val();
    a->write("rel8test: val="); a->print_int(v); a->write("\n");
    a->dlclose(h);

    if (v == 42) {
        a->write("rel8test: PASS\n");
        return 0;
    }
    a->write("rel8test: FAIL\n");
    return 4;
}
