#include "ce_abi.h"

catos_api_t *__ce_api __attribute__((section(".ce_api"))) = (catos_api_t *)0;


/* spinner.ce - multitasking proof: prints a line every 500 ms for ~5 s.
 *   spinner &           (background, then use the shell while it runs)
 * Uses delay() - which is a real sleeping syscall now, so `ps` shows the
 * task in SLEEP state between ticks instead of burning CPU.
 */
__attribute__((section(".text.ce_entry")))
int ce_main(int argc, char **argv) {
    catos_api_t *api = CE_API();
    int n = 8;
    if (argc >= 2) {
        int v = api->atoi(argv[1]);
        if (v > 0 && v <= 60) n = v;
    }
    api->write("[spinner tid=");
    api->print_int(api->gettid());
    api->write(" start, ");
    api->print_int(n);
    api->write(" ticks]\n");
    for (int i = 1; i <= n; i++) {
        api->delay(500);
        api->write("[spinner tid=");
        api->print_int(api->gettid());
        api->write(" tick ");
        api->print_int(i);
        api->write("/");
        api->print_int(n);
        api->write("]\n");
    }
    api->write("[spinner done]\n");
    return 0;
}
