/* sink_ce.c - pipe consumer: reads lines from stdin (readline is wired to
 * the pipe when spawned with `A | B`), counts them, prints the total. */
#include "ce_abi.h"
catos_api_t *__ce_api __attribute__((section(".ce_api")));
__attribute__((section(".text.ce_entry")))
int ce_main(int argc, char **argv) {
    catos_api_t *a = CE_API();
    (void)argc; (void)argv;
    char line[64];
    int count = 0;
    for (;;) {
        int r = a->readline(line, 64);
        if (r <= 0) break;
        /* readline returns 1; EOF comes back as an empty line when the
         * writer has closed (kernel returns empty buffer) */
        if (line[0] == 0) break;
        count++;
    }
    a->write("SINK got ");
    a->print_int(count);
    a->write(" lines\n");
    return 0;
}
