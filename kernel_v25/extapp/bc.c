/* bc.c - P4 package: tiny non-interactive calculator for CatOS.
 *
 * Usage (from the shell after `pkg install bc`):
 *     bc 12 + 34
 *     bc 7 * 8
 *     bc 100 / 3
 *     bc 17 % 5
 *
 * Supports operators + - * / % on integers up to 6 digits.  This is a
 * packaged CE app: it is NOT in build/apps.list, so it ships in /pkg/
 * and only lands in /bin after `pkg install bc`.
 */
#include "ce_abi.h"

catos_api_t *__ce_api __attribute__((section(".ce_api"))) = (catos_api_t *)0;

static long parse_num(const char **pp) {
    const char *p = *pp;
    long v = 0;
    int neg = 0;
    if (*p == '-') { neg = 1; p++; }
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
    *pp = p;
    return neg ? -v : v;
}

static void skip_sp(const char **pp) {
    while (**pp == ' ') (*pp)++;
}

__attribute__((section(".text.ce_entry")))
int ce_main(int argc, char **argv) {
    catos_api_t *api = CE_API();
    char line[64];
    line[0] = 0;

    if (argc > 1) {
        int i = 0;
        char *d = line;
        for (int k = 1; k < argc && i < (int)sizeof(line) - 2; k++) {
            const char *s = argv[k] ? argv[k] : "";
            while (*s && i < (int)sizeof(line) - 2) { *d++ = *s++; i++; }
            *d = 0;
            if (k < argc - 1 && i < (int)sizeof(line) - 2) { *d++ = ' '; i++; }
        }
    }

    const char *p = line;
    skip_sp(&p);
    long a = parse_num(&p);
    skip_sp(&p);
    char op = *p;
    if (op) p++;
    skip_sp(&p);
    long b = parse_num(&p);
    skip_sp(&p);

    if (!*line || !op || *p) {
        api->write("usage: bc A op B   (op = + - * / %)\n");
        return 1;
    }

    long r;
    switch (op) {
    case '+': r = a + b; break;
    case '-': r = a - b; break;
    case '*': r = a * b; break;
    case '/':
        if (b == 0) { api->write("bc: division by zero\n"); return 1; }
        r = a / b; break;
    case '%':
        if (b == 0) { api->write("bc: division by zero\n"); return 1; }
        r = a % b; break;
    default:
        api->write("bc: bad operator (use + - * / %)\n");
        return 1;
    }

    api->print_int((int)a);
    api->putc(' '); api->putc(op); api->putc(' ');
    api->print_int((int)b);
    api->write(" = ");
    api->print_int((int)r);
    api->write("\n");
    return 0;
}
