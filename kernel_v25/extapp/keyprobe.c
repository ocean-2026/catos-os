/* keyprobe.c - diagnostic: does pollkey()/getch() see keys in a CE task?
 * Prototype for the CDE keyboard-path investigation. Prints each decoded
 * key as a decimal value; press q to exit. */
#include "ce_app.h"

static void puti(catos_api_t *api, int v) {
    char b[16]; int i = 0, n;
    unsigned u;
    if (v < 0) { api->write("-"); u = (unsigned)(-v); }
    else u = (unsigned)v;
    if (u == 0) b[i++] = '0';
    while (u > 0) { b[i++] = (char)('0' + (u % 10)); u /= 10; }
    n = i;
    while (i > 0) api->write(&b[--i]);   /* write one char at a time */
    (void)n;
}

__attribute__((section(".text.ce_entry")))
int ce_main(int argc, char **argv) {
    catos_api_t *api = CE_API();
    (void)argc; (void)argv;
    api->write("KEYPROBE: blocking getch test. press a key\n");
    if (!api->getch) { api->write("NO GETCH SLOT\n"); return 1; }
    {
        int k = api->getch();          /* BLOCKING: proves IRQ1->scq path */
        api->write("getch=");
        puti(api, k);
        api->write("\n");
    }
    api->write("KEYPROBE: pollkey test. press keys (q quits)\n");
    for (;;) {
        int k = api->pollkey();
        if (k >= 0) {
            api->write("pollkey=");
            puti(api, k);
            api->write("\n");
            if (k == 'q') { api->write("KEYPROBE: got q, exiting\n"); return 0; }
        } else {
            if (api->delay) api->delay(50);
        }
    }
}
