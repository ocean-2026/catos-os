#include "ce_abi.h"

catos_api_t *__ce_api __attribute__((section(".ce_api"))) = (catos_api_t *)0;

/* entry point: must be at image offset 0 (see ce.ld / build_ce.sh).
 * argc/argv carry the command line: `run /bin/hello.ce x y` */
__attribute__((section(".text.ce_entry")))
int ce_main(int argc, char **argv) {
    catos_api_t *api = CE_API();
    char t[24];
    (void)argc; (void)argv;

    api->write("Hello from CE!\n");
    api->write("This is a native CatOS application.\n");
    api->write("Time: ");
    api->time_str(t);
    api->write(t);
    api->write("\n");

    /* exercise memory allocation + exit path */
    {
        char *p = (char *)api->alloc(64);
        if (p) {
            int i;
            for (i = 0; i < 10; i++) p[i] = (char)('a' + i);
            p[10] = 0;
            api->write("alloc test: ");
            api->write(p);
            api->write("\n");
            api->free(p);
        } else {
            api->write("alloc test: out of memory\n");
        }
    }
    return 0;
}
