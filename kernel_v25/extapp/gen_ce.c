/* gen_ce.c - pipe producer: writes 10 lines via CE_API()->write (which the
 * kernel redirects to the pipe when spawned with `A | B`). */
#include "ce_abi.h"
catos_api_t *__ce_api __attribute__((section(".ce_api")));
__attribute__((section(".text.ce_entry")))
int ce_main(int argc, char **argv) {
    catos_api_t *a = CE_API();
    (void)argc; (void)argv;
    for (int i = 0; i < 10; i++) {
        char b[24];
        int n = 0;
        b[n++] = 'L';
        if (i >= 10) { b[n++] = '0' + i / 10; }
        b[n++] = '0' + i % 10;
        b[n++] = ':';
        b[n++] = 'x';
        b[n++] = '\n';
        a->write(b);
    }
    a->write("EOF\n");
    return 0;
}
