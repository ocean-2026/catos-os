/* stats.c - P4 package: text statistics (lines / words / bytes).
 *
 * Usage:  stats FILE
 * Prints: lines words bytes  FILE   (POSIX-style wc summary).
 *
 * Packaged CE app: ships in /pkg/, installed to /bin by `pkg install stats`.
 */
#include "ce_abi.h"

catos_api_t *__ce_api __attribute__((section(".ce_api"))) = (catos_api_t *)0;

__attribute__((section(".text.ce_entry")))
int ce_main(int argc, char **argv) {
    catos_api_t *api = CE_API();
    if (argc < 2 || !argv[1] || !*argv[1]) {
        api->write("usage: stats FILE\n");
        return 1;
    }
    const char *path = argv[1];
    int sz = api->fsize(path);
    if (sz < 0) {
        api->write("stats: no such file: ");
        api->write(path); api->write("\n");
        return 1;
    }

    char buf[1024];
    int lines = 0, words = 0, bytes = 0;
    int in_word = 0;
    int off = 0;
    while (off < sz) {
        int want = sizeof(buf);
        if (want > sz - off) want = sz - off;
        int n = api->file_read(path, buf, want, off);
        if (n <= 0) break;
        for (int i = 0; i < n; i++) {
            char c = buf[i];
            bytes++;
            if (c == '\n') lines++;
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                in_word = 0;
            } else if (!in_word) {
                in_word = 1;
                words++;
            }
        }
        off += n;
    }
    if (bytes > 0 && buf[bytes - 1] != '\n') lines++;

    api->print_int(lines); api->write("  ");
    api->print_int(words); api->write("  ");
    api->print_int(bytes); api->write("  ");
    api->write(path); api->write("\n");
    return 0;
}
