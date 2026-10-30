#include "ce_abi.h"
catos_api_t *__ce_api __attribute__((section(".ce_api")));
__attribute__((section(".text.ce_entry")))
int ce_main(int argc, char **argv) {
    catos_api_t *a = CE_API();
    (void)argc; (void)argv;
    a->write("MINIMAL OK\n");
    return 0;
}
