#include "ce_abi.h"

/* hw.ce - ring3 demo of the v2.4 hardware-properties syscalls
 * (CatOS' HardwarePropertiesManager).   run:  hw
 */
catos_api_t *__ce_api __attribute__((section(".ce_api"))) = (catos_api_t *)0;

/* hw_num / hw_str selectors (must match include/hw.h) */
#define HN_CPU_MHZ   1
#define HN_CPU_CORES 2
#define HN_MEM_TOTAL 6
#define HN_MEM_FREE  7
#define HN_MEM_USED  8
#define HN_DISK_MB   10
#define HN_UPTIME    13
#define HN_TASKS     14
#define HS_CPU_BRAND 2
#define HS_CPU_FLAGS 3
#define HS_DISK_MODEL 4
#define HS_NIC_MODEL 7
#define HS_NIC_MAC   8
#define HS_NIC_IP    9
#define HS_PCI_LIST  10

static void num(catos_api_t *api, const char *label, int what) {
    api->write(label);
    api->print_int(api->hw_num(what));
    api->write("\n");
}
static void strq(catos_api_t *api, const char *label, int what) {
    static char b[2048];
    api->write(label);
    api->hw_str(what, b, sizeof(b));
    api->write(b);
    api->write("\n");
}

__attribute__((section(".text.ce_entry")))
int ce_main(int argc, char **argv) {
    catos_api_t *api = CE_API();
    (void)argc; (void)argv;
    api->write("== hardware properties from ring3 (tid ");
    api->print_int(api->gettid());
    api->write(", uid ");
    api->print_int(api->getuid());
    api->write(") ==\n");
    strq(api, "cpu.brand : ", HS_CPU_BRAND);
    strq(api, "cpu.flags : ", HS_CPU_FLAGS);
    num (api, "cpu.mhz   : ", HN_CPU_MHZ);
    num (api, "cpu.cores : ", HN_CPU_CORES);
    num (api, "mem.total : ", HN_MEM_TOTAL);
    num (api, "mem.free  : ", HN_MEM_FREE);
    num (api, "mem.used  : ", HN_MEM_USED);
    strq(api, "disk.model: ", HS_DISK_MODEL);
    num (api, "disk.mb   : ", HN_DISK_MB);
    strq(api, "nic.model : ", HS_NIC_MODEL);
    strq(api, "nic.mac   : ", HS_NIC_MAC);
    strq(api, "nic.ip    : ", HS_NIC_IP);
    num (api, "uptime.ms : ", HN_UPTIME);
    num (api, "tasks     : ", HN_TASKS);
    api->write("pci devices:\n");
    strq(api, "", HS_PCI_LIST);
    api->write("[hw done]\n");
    return 0;
}
