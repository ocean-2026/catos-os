#include "ce_abi.h"

catos_api_t *__ce_api __attribute__((section(".ce_api"))) = (catos_api_t *)0;

/* netinfo.ce - explore the CatOS network: own IP, DNS, ping, UDP echo.
 *   run /bin/netinfo.ce
 */
__attribute__((section(".text.ce_entry")))
int ce_main(int argc, char **argv) {
    catos_api_t *api = CE_API();
    char ip[24];
    (void)argc; (void)argv;

    api->set_fg(2);
    api->write("=== CatOS netinfo ===\n");
    api->set_fg(7);

    if (api->net_init() < 0) {
        api->write("network: no network card found (add -device pcnet)\n");
        return 0;
    }
    if (!api->net_up()) {
        api->write("network: adapter not ready\n");
        return 0;
    }

    api->net_ip(ip);
    api->write("my ip  : ");
    api->write(ip);
    api->write("\n");

    api->write("resolve 'example.com' ...\n");
    if (api->net_dns("example.com", ip) == 0) {
        api->write("example.com -> ");
        api->write(ip);
        api->write("\n");
    } else {
        api->write("dns: failed (host may be offline)\n");
    }

    api->write("ping 10.0.2.2 ...\n");
    if (api->net_ping("10.0.2.2") == 0)
        api->write("ping 10.0.2.2: reply\n");
    else
        api->write("ping 10.0.2.2: no reply\n");

    api->write("udp echo to 10.0.2.2:9999 ...\n");
    if (api->net_udp_send("10.0.2.2", 9999, "hello from CatOS", 17) == 0) {
        char buf[512];
        char rip[24];
        int rport = 0;
        int n = api->net_udp_recv(rip, &rport, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = 0;
            api->write("reply from ");
            api->write(rip);
            api->write(":");
            api->print_int(rport);
            api->write(": ");
            api->write(buf);
            api->write("\n");
        } else {
            api->write("udp: no reply (start an echo server on the host)\n");
        }
    } else {
        api->write("udp: send failed\n");
    }

    api->write("done\n");
    return 0;
}
