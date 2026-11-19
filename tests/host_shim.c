/* host_shim.c -- run cc.c's ce_main on the host (x86-64 friendly).
 * Provides a fake catos_api_t whose file I/O hits the real filesystem,
 * so cc.ce can be exercised without QEMU or a full i386 emulator. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

typedef struct catos_api {
    void (*write)(const char *s);
    void (*putc_)(char c);
    void (*cls)(void);
    int  (*readline)(char *b, int m);
    void (*time_str)(char *o);
    int  (*fs_ready)(void);
    void *ls; void *touch; void *mkdir; void *cat; void *save;
    void *alloc; void *free_;
    void (*exit_)(int c);
    void (*print_int)(int v);
    void (*print_hex)(unsigned v);
    void *set_fg; void *delay; void *atoi_; void *strlen_; void *strcmp_;
    void *strcpy_ ; void *getch; void *file_exists_; void *fsize_;
    void *net_init; void *net_up; void *http_get; void *net_dns;
    void *net_udp_send; void *net_udp_recv; void *net_ping; void *net_ip;
    void *getuid_; void *gettid; void *kill_; void *yield_;
    void *hw_num; void *hw_str;
    int  (*file_read)(const char *p, char *b, int max, int off);
    int  (*file_write)(const char *p, const char *d, int n, int mode);
    void *file_delete; void *pipe_create; void *pipe_write; void *pipe_read;
    void *pipe_close; void *spawn; void *wait_;
} catos_api_t;

static catos_api_t g_api;

static void h_write(const char *s) { fputs(s, stdout); }
static void h_putc(char c) { fputc(c, stdout); }
static void h_print_int(int v) { printf("%d", v); }
static void h_print_hex(unsigned v) { printf("0x%08x", v); }
/* api->alloc / free: the compiler draws its working set from the user
 * heap.  On the host these are plain malloc/free. */
static void *h_alloc(uint32_t n) { return malloc(n); }
static void h_free(void *p) { free(p); }

static int h_file_read(const char *p, char *b, int max, int off) {
    FILE *f = fopen(p, "rb");
    if (!f) return -1;
    fseek(f, off, SEEK_SET);
    int n = (int)fread(b, 1, (size_t)max, f);
    fclose(f);
    return n;
}
static int h_file_write(const char *p, const char *d, int n, int mode) {
    FILE *f = fopen(p, mode == 0 ? "wb" : "ab");
    if (!f) return -1;
    fwrite(d, 1, (size_t)n, f);
    fclose(f);
    return 0;
}

catos_api_t *__ce_api = &g_api;

/* cc.c defines ce_main; declare it here and call it from main() */
int ce_main(int argc, char **argv);

static void init(void) {
    memset(&g_api, 0, sizeof(g_api));
    g_api.write = h_write;
    g_api.putc_ = h_putc;
    g_api.print_int = h_print_int;
    g_api.print_hex = h_print_hex;
    g_api.alloc = h_alloc;
    g_api.free_ = h_free;
    g_api.file_read = h_file_read;
    g_api.file_write = h_file_write;
}

int main(int argc, char **argv) {
    init();
    return ce_main(argc, argv);
}
