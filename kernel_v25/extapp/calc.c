#include "ce_abi.h"

catos_api_t *__ce_api __attribute__((section(".ce_api"))) = (catos_api_t *)0;

/* a <op> b  with a/b up to 6 digits, ops: + - * / % */
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

static void eval_one(catos_api_t *api, const char *line) {
    const char *p = line;
    skip_sp(&p);
    long a = parse_num(&p);
    skip_sp(&p);
    char op = *p;
    if (op) p++;
    skip_sp(&p);
    long b = parse_num(&p);
    skip_sp(&p);

    if (!op || *p) {
        api->write("syntax: A op B\n");
        return;
    }

    switch (op) {
    case '+': api->print_int((int)(a + b)); break;
    case '-': api->print_int((int)(a - b)); break;
    case '*': api->print_int((int)(a * b)); break;
    case '/':
        if (b == 0) { api->write("div by zero\n"); return; }
        api->print_int((int)(a / b)); break;
    case '%':
        if (b == 0) { api->write("div by zero\n"); return; }
        api->print_int((int)(a % b)); break;
    default:
        api->write("bad operator\n");
        return;
    }
    api->write("\n");
}

__attribute__((section(".text.ce_entry")))
int ce_main(int argc, char **argv) {
    catos_api_t *api = CE_API();
    char line[64];

    api->write("CatOS Calculator\n");
    api->write("expr: A op B   (op = + - * / %)\n");

    /* command-line mode: run /bin/calc.ce 10 + 5 */
    if (argc > 1) {
        int i = 0;
        char *d = line;
        for (int k = 1; k < argc && i < (int)sizeof(line) - 2; k++) {
            const char *s = argv[k] ? argv[k] : "";
            while (*s && i < (int)sizeof(line) - 2) { *d++ = *s++; i++; }
            *d = 0;
            if (k < argc - 1 && i < (int)sizeof(line) - 2) { *d++ = ' '; i++; }
        }
        eval_one(api, line);
        api->write("bye\n");
        return 0;
    }

    api->write("type 'quit' to return to the shell\n");
    for (;;) {
        api->write("calc> ");
        if (api->readline(line, sizeof(line)) <= 0) break;

        if (api->strcmp(line, "quit") == 0 || api->strcmp(line, "exit") == 0)
            break;

        eval_one(api, line);
    }
    api->write("bye\n");
    return 0;
}
