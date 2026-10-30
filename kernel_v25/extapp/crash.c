#include "ce_abi.h"

catos_api_t *__ce_api __attribute__((section(".ce_api"))) = (catos_api_t *)0;


/* crash.ce - isolation proof: deliberately touch KERNEL memory from ring3.
 * Expected: the task is killed by a page fault (kernel pages are
 * supervisor-only in the task's page directory), the system survives.
 *   crash kernel   -> write to 0x1000000 (kernel .text)   -> killed
 *   crash null     -> write to address 0                  -> killed
 *   crash ok       -> touches only its own memory         -> exits 0
 */
__attribute__((section(".text.ce_entry")))
int ce_main(int argc, char **argv) {
    catos_api_t *api = CE_API();
    const char *mode = (argc >= 2) ? argv[1] : "kernel";
    api->write("[crash] mode=");
    api->write(mode);
    api->write(" (tid ");
    api->print_int(api->gettid());
    api->write(")\n");

    volatile unsigned *p;
    if (api->strcmp(mode, "kernel") == 0) {
        p = (volatile unsigned *)0x1000100u;   /* kernel image */
    } else if (api->strcmp(mode, "null") == 0) {
        p = (volatile unsigned *)0x10u;        /* null page */
    } else {
        api->write("[crash] behaving, nothing to see\n");
        return 0;
    }
    api->write("[crash] writing now... if you see this and then a kill\n");
    api->write("[crash] message with the system still alive, isolation works.\n");
    *p = 0xdeadbeef;
    api->write("[crash] NOT REACHED (write succeeded?!)\n");
    return 0;
}
