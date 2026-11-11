#include "ce_abi.h"

catos_api_t *__ce_api __attribute__((section(".ce_api"))) = (catos_api_t *)0;

/* captest.ce - capability-gate probe (v2.13).
 * Prints the RAW return codes of privileged syscalls so the regression can
 * distinguish "no hardware" from "denied by capability":
 *   root  (CAP_ALL):  net_init=-1   (no NIC in QEMU),  snd=0 (absent card)
 *   guest (caps=0) :  net_init=-13, http=-13, snd=-13  (capability denial)
 * Anything else means the gate regressed. */
__attribute__((section(".text.ce_entry")))
int ce_main(int argc, char **argv) {
    (void)argc; (void)argv;
    catos_api_t *api = CE_API();
    api->write("[captest] uid=");
    api->print_int(api->getuid());
    api->write(" net_init=");
    api->print_int(api->net_init());
    api->write(" http=");
    static char buf[64];
    api->print_int(api->http_get("http://10.0.2.2:9/probe", buf, sizeof(buf)));
    api->write(" snd=");
    api->print_int(api->snd_info(0));
    api->write("\n");
    return 0;
}
