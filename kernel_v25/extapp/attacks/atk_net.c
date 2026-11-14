/* atk_net.c -- attack vector: the network syscalls (SYS_HTTP_GET /
 * NET_DNS / NET_UDP_SEND / NET_UDP_RECV / NET_PING / NET_IP).
 *
 * WHY IT MIGHT CRASH
 *   Every one of these takes a user pointer plus a length and runs a
 *   protocol parser (URL split, DNS name build, UDP payload).  Malformed
 *   input (no scheme, ':' with nothing after it, a 300-byte host name, a
 *   negative length, max = INT_MAX) is the classic way to walk off a
 *   stack buffer inside the netstack.
 *
 * EXPECTED NEGATIVE RESULT: this guest boots with `-net none`, so there is
 * no PCnet device; net_up() == 0 and every call should fail with an error
 * code before it ever builds a packet.  A crash here would therefore be a
 * validation bug that runs BEFORE the NIC check -- which is exactly what
 * we want to rule out.
 */
#include "atk_common.h"

catos_api_t *__ce_api __attribute__((section(".ce_api"))) = 0;

static char buf[9000];
static char host[400];

__attribute__((section(".text.ce_entry")))
int ce_main(int argc, char **argv) {
    catos_api_t *api = A();
    int i;
    (void)argc; (void)argv;

    w("ATK-NET: netstack input abuse (guest has -net none)\n");

    step(1); mark("nic state\n");
    m("init", api->net_init());
    m("up", api->net_up());
    api->net_ip(buf);
    w("ip="); w(buf); nl();

    step(2); mark("http_get: malformed urls, huge max\n");
    {
        static const char *U[] = {
            "", "x", ":", "://", "http://", "http://:", "http://:0/",
            "http://[::]/", "GET / HTTP/1.1\r\n\r\n",
            "http://averyveryveryveryveryverylonghostnamethatcannotresolve/",
            "file:///etc/passwd", "/BIN/HELLO.CE",
        };
        for (i = 0; i < (int)(sizeof(U) / sizeof(U[0])); i++) {
            wd(api->http_get(U[i], buf, 64)); w(" ");
        }
        nl();
        m("max-1", api->http_get("http://127.0.0.1/", buf, -1));
        m("max0", api->http_get("http://127.0.0.1/", buf, 0));
        m("max-max", api->http_get("http://127.0.0.1/", buf, 0x7FFFFFFF));
        m("buf-max", api->http_get("http://127.0.0.1/", (char *)0xFFFFFFFFu, 64));
        m("buf-vram", api->http_get("http://127.0.0.1/", (char *)0xB8000u, 64));
        m("url-max", api->http_get((const char *)0xFFFFFFFFu, buf, 64));
        nl();
    }

    step(3); mark("net_dns: 398-char host, dots, no NUL path\n");
    for (i = 0; i < 398; i++) host[i] = (i % 40 == 39) ? '.' : 'a';
    host[398] = 0;
    m("dns(398)", api->net_dns(host, buf));
    m("dns(empty)", api->net_dns("", buf));
    m("dns(dots)", api->net_dns(".......", buf));
    m("dns(max)", api->net_dns((const char *)0xFFFFFFFFu, buf));
    m("dns(out-max)", api->net_dns("a.b", (char *)0xFFFFFFFFu));
    nl();

    step(4); mark("udp_send/recv length abuse\n");
    m("send(-1)", api->net_udp_send("10.0.2.2", 9, buf, -1));
    m("send(0)", api->net_udp_send("10.0.2.2", 9, buf, 0));
    m("send(1400)", api->net_udp_send("10.0.2.2", 9, buf, 1400));
    m("send(1401)", api->net_udp_send("10.0.2.2", 9, buf, 1401));
    m("send(max)", api->net_udp_send("10.0.2.2", 9, buf, 0x7FFFFFFF));
    m("send(badip)", api->net_udp_send("999.999.999.999", 9, buf, 8));
    m("send(poison)", api->net_udp_send((const char *)0xFFFFFFFFu, 9,
                                        (const char *)0xFFFFFFFFu, 8));
    m("recv(-1)", api->net_udp_recv(buf, (int *)buf, buf, -1));
    m("recv(max)", api->net_udp_recv(buf, (int *)buf, buf, 0x7FFFFFFF));
    m("recv(poison)", api->net_udp_recv((char *)0xFFFFFFFFu, (int *)0xFFFFFFFFu,
                                        (char *)0xFFFFFFFFu, 64));
    nl();

    step(5); mark("ping abuse\n");
    m("ping('')", api->net_ping(""));
    m("ping(max)", api->net_ping((const char *)0xFFFFFFFFu));
    m("ping(300)", api->net_ping(host));
    nl();

    w("\nATK-DONE\n");
    return 0;
}
