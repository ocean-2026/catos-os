#include "ce_abi.h"

catos_api_t *__ce_api __attribute__((section(".ce_api"))) = (catos_api_t *)0;

/* curl.ce - tiny HTTP client using the CatOS network API.
 *   run /bin/curl.ce http://10.0.2.2:8080/API.txt   (URL as argument)
 *   run /bin/curl.ce                                 (then type an URL)
 */
__attribute__((section(".text.ce_entry")))
int ce_main(int argc, char **argv) {
    catos_api_t *api = CE_API();
    char url[160];
    char buf[1024];

    api->set_fg(2);
    api->write("=== CatOS curl ===\n");
    api->set_fg(7);

    if (api->net_init() < 0) {
        api->write("network: no network card found (add -device pcnet)\n");
        return 0;
    }
    if (!api->net_up()) {
        api->write("network: adapter not ready\n");
        return 0;
    }
    api->write("net: up (10.0.2.15)\n");

    if (argc > 1 && argv[1] && argv[1][0]) {
        api->strcpy(url, argv[1]);          /* URL came from the command line */
    } else {
        api->write("URL> ");
        if (api->readline(url, sizeof(url)) <= 0) return 0;
    }

    api->write("fetching ...\n");
    int n = api->http_get(url, buf, sizeof(buf));
    if (n < 0) {
        api->write("curl: error ");
        api->print_int(n);
        api->write(" (1=NIC 2=host 3=connect 4=http 5=resp 6=send)\n");
        return 0;
    }
    api->write("--- response ---\n");
    api->write(buf);
    api->write("\n--- done: ");
    api->print_int(n);
    api->write(" bytes ---\n");
    return 0;
}
