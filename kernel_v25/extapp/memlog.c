/* memlog.c - v2.54 demonstration daemon: periodic memory watermark logger.
 *
 * Usage:  memlog [ticks] [interval_ms]
 *   ticks        samples to take before exiting (default 12)
 *   interval_ms  sleep between samples (default 1000)
 *
 * Launched detached:  `daemon memlog`  (or an `@memlog` line in /etc/autoexec).
 * Each sample prints a one-line watermark to its captured output ring, which
 * the shell reads back via `jobs` / `log <tid>`.  The loop sleeps on the
 * timer wake source; there is no busy poll.  Stop it with `kill <tid>`.
 */
#include "ce_app.h"

__attribute__((section(".text.ce_entry")))
int ce_main(int argc, char **argv) {
    catos_api_t *api = CE_API();
    int ticks = 12;
    int interval = 1000;
    if (argc >= 2) ticks = api->atoi(argv[1]);
    if (argc >= 3) interval = api->atoi(argv[2]);
    if (ticks < 1) ticks = 1;
    if (interval < 50) interval = 50;

    api->write("[memlog tid=");
    api->print_int(api->gettid());
    api->write(" daemon up, ");
    api->print_int(ticks);
    api->write(" samples @ ");
    api->print_int(interval);
    api->write("ms]\n");

    for (int i = 1; i <= ticks; i++) {
        api->delay(interval);
        void *brk = api->brk(0);            /* current heap break (query) */
        api->write("[memlog ");
        api->print_int(i);
        api->write("/");
        api->print_int(ticks);
        api->write(" heap=0x");
        api->print_hex((uint32_t)brk);
        api->write("]\n");
    }
    api->write("[memlog done]\n");
    return 0;
}
