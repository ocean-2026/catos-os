/*
 * host_stub_go.c -- host (gcc) shim to build go.c as a native binary.
 *
 * Ported from the original project's build/host_stub_go.c (which mirrored
 * the then-current 48-slot CE ABI by hand).  The current kernel ships a
 * 57-slot table (include/ce_abi.h, v2.10), so this stub now includes the
 * REAL header instead of re-declaring the struct: the layout can then
 * never drift from the kernel's, and every slot the interpreter can
 * reach has a matching host implementation.
 *
 * Build (from kernel_v25/extapp/):
 *   gcc -O1 -g -I ../include \
 *       host_stub_go.c go.c -o go_host
 *
 * Run:
 *   ./go_host [script.GO [arg2 ...]]
 *
 * Paths handed to go.ce (e.g. /BIN/TEST.GO) are mapped: a leading "/BIN/"
 * is stripped so the host looks the file up in the current directory.
 * That lets you run "./go_host /BIN/TEST.GO" against ./TEST.GO in cwd.
 *
 * PORTABILITY NOTE -- why this file uses only ANSI C file I/O:
 * compiling with "-I kernel_v25/include" puts the kernel's own <io.h>
 * (a port-I/O helper header) ahead of the C library's <io.h>, so the
 * POSIX file-descriptor API (open/read/write/lseek/access/_open/_close)
 * is NOT declared and the build breaks with "implicit declaration".
 * Rather than fight the include order, the whole file layer below is
 * written against ANSI C stdio (fopen/fread/fwrite/fseek/ftell/remove)
 * plus <sys/stat.h> for stat()/mkdir(), none of which needs <io.h>.
 *
 * The stub is a development aid only: nothing in the kernel image depends
 * on it, and it never touches the real FAT32 volume.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#if defined(_WIN32) || defined(__MINGW32__)
#include <direct.h>   /* _mkdir  (pulls the C library's own <io.h>) */
#include <sys/stat.h> /* stat() */
#define HOST_MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h> /* stat(), mkdir() */
#include <unistd.h>
#define HOST_MKDIR(p) mkdir((p), 0755)
#endif

/* The real kernel-side ABI: 57 slots, v2.10.  Including the header (not
 * copying it) is what keeps this stub honest. */
#include "ce_abi.h"

/* go.c defines __ce_api (initialized to 0); we overwrite it in main(). */
extern catos_api_t *__ce_api;

/* entry called by go.c */
extern int ce_main(int argc, char **argv);

/* ---- console ---- */
static void h_write(const char *s) { fputs(s, stdout); }
static void h_putc(char c)        { fputc(c, stdout); }
static void h_cls(void)            { fprintf(stdout, "\x1b[2J\x1b[H"); }

/* ---- input ---- */
static int h_readline(char *buf, int max) {
    if (!fgets(buf, max, stdin)) return 0;
    return (int)strlen(buf);
}

/* ---- time ---- */
static void h_time_str(char *out) { strcpy(out, "2026-01-01 00:00:00"); }

/* ---- filesystem ---- */
static int h_fs_ready(void) { return 1; }
static int h_ls(const char *p, ce_ls_cb cb, void *ud) {
    (void)p; (void)cb; (void)ud;
    return -1;
}
static int h_touch(const char *p) {
    FILE *f = fopen(p, "wb");
    if (!f) return -1;
    fclose(f);
    return 0;
}
static int h_mkdir(const char *p) { return HOST_MKDIR(p) == 0 ? 0 : -1; }
static int h_cat(const char *p, char *buf, int max) {
    FILE *f = fopen(p, "rb");
    if (!f) return -1;
    size_t n = fread(buf, 1, (size_t)(max - 1), f);
    fclose(f);
    buf[n] = 0;
    return (int)n;
}
static int h_save(const char *p, const char *data, uint32_t size) {
    FILE *f = fopen(p, "wb");
    if (!f) return -1;
    size_t n = fwrite(data, 1, (size_t)size, f);
    fclose(f);
    return (int)n;
}

/* ---- memory ---- */
/* Test hook: GO_HOST_ALLOC_FAIL=1 makes every api->alloc() return NULL, so
 * go.c takes its static-arena fallback and eventually hits the FIND-G6
 * out-of-memory guards.  Used by the FIND-G verification suite. */
static int h_alloc_fail(void) {
    static int checked = 0;
    if (!checked) {
        checked = 1;
        const char *e = getenv("GO_HOST_ALLOC_FAIL");
        if (e && e[0] == '1') return 1;
    }
    return 0;
}
static void *h_alloc(uint32_t n) {
    if (h_alloc_fail()) return 0;
    return malloc(n ? n : 1);
}
static void  h_free(void *p)     { free(p); }
static void  h_exit(int c)       { exit(c); }

/* ---- beginner-friendly helpers ---- */
static void h_print_int(int v)      { printf("%d", v); }
static void h_print_hex(uint32_t v) { printf("0x%08x", v); }
static void h_set_fg(int c)         { (void)c; }
static void h_delay(uint32_t ms)    {
    /* The CE slot is documented as a busy-wait, so a CPU-time spin is the
     * faithful host equivalent (and needs no OS sleep primitive). */
    clock_t target = clock() + (clock_t)((ms * CLOCKS_PER_SEC) / 1000u);
    while (clock() < target) { }
}
static int  h_atoi(const char *s)   { return atoi(s); }
static int  h_strlen(const char *s) { return (int)strlen(s); }
static int  h_strcmp(const char *a, const char *b) { return strcmp(a, b); }
static void h_strcpy(char *d, const char *s) { strcpy(d, s); }
static int  h_getch(void) { return -1; }

/* Map /BIN/FOO.GO -> ./FOO.GO ; otherwise use the path as-is. */
static const char *h_map(const char *path, char *tmp, int tlen) {
    if (strncmp(path, "/BIN/", 5) == 0) {
        snprintf(tmp, (size_t)tlen, "./%s", path + 5);
        return tmp;
    }
    if (strcmp(path, "/BIN") == 0 || strcmp(path, "/") == 0) {
        snprintf(tmp, (size_t)tlen, ".");
        return tmp;
    }
    return path;
}

static int h_file_exists(const char *path) {
    char tmp[512];
    const char *p = h_map(path, tmp, (int)sizeof tmp);
    struct stat st;
    if (stat(p, &st) == 0) return 1;
    return 0;
}
static int h_fsize(const char *path) {
    char tmp[512];
    const char *p = h_map(path, tmp, (int)sizeof tmp);
    struct stat st;
    if (stat(p, &st) != 0) return -1;
    return (int)st.st_size;
}
static int h_file_read(const char *path, char *buf, int max, int offset) {
    /* The kernel enforces max <= 16384 per call (see include/ce_abi.h);
     * the host clamps the same way so the chunked read path is exercised
     * exactly as it is on metal. */
    if (max > 16384) max = 16384;
    if (max <= 0) return -3;
    char tmp[512];
    const char *p = h_map(path, tmp, (int)sizeof tmp);
    FILE *f = fopen(p, "rb");
    if (!f) return -1;
    if (offset < 0) { fclose(f); return -3; }
    if (fseek(f, (long)offset, SEEK_SET) != 0) { fclose(f); return -3; }
    size_t n = fread(buf, 1, (size_t)max, f);
    fclose(f);
    return (int)n;
}
static int h_file_write(const char *path, const char *data, int size, int mode) {
    if (size > 16384) size = 16384;
    if (size < 0) return -1;
    char tmp[512];
    const char *p = h_map(path, tmp, (int)sizeof tmp);
    FILE *f = fopen(p, (mode == 1) ? "ab" : "wb");
    if (!f) return -1;
    size_t n = fwrite(data, 1, (size_t)size, f);
    fclose(f);
    return (int)n;
}
static int h_file_delete(const char *path) {
    char tmp[512];
    const char *p = h_map(path, tmp, (int)sizeof tmp);
    return remove(p) == 0 ? 0 : -1;
}

/* ---- pipes / processes: not modelled on the host ---- */
static int h_pipe_create(void)              { return -1; }
static int h_pipe_write(int h, const char *d, int l) { (void)h; (void)d; (void)l; return -1; }
static int h_pipe_read(int h, char *b, int m)         { (void)h; (void)b; (void)m; return -1; }
static int h_pipe_close(int h)                        { (void)h; return -1; }
static int h_spawn(const char *p, int ac, char **av)  { (void)p; (void)ac; (void)av; return -1; }
static int h_wait(int t)                              { (void)t; return -1; }

/* ---- networking ---- */
static int  h_net_init(void)              { return -1; }
static int  h_net_up(void)                { return 0; }
static int  h_http_get(const char *u, char *b, int m) { (void)u; (void)b; (void)m; return -1; }
static int  h_net_dns(const char *h, char *o) { (void)h; (void)o; return -1; }
static int  h_net_udp_send(const char *ip, int p, const char *d, int l) {
    (void)ip; (void)p; (void)d; (void)l; return -1;
}
static int  h_net_udp_recv(char *ip, int *p, char *b, int m) {
    (void)ip; (void)p; (void)b; (void)m; return -1;
}
static int  h_net_ping(const char *ip)    { (void)ip; return -1; }
static void h_net_ip(char *o)             { strcpy(o, "0.0.0.0"); }

/* ---- task / hardware ---- */
static int  h_getuid(void) { return 0; }
static int  h_gettid(void) { return 1; }
static int  h_kill(int t)  { (void)t; return -1; }
static void h_yield(void)  { }
static int  h_hw_num(int w)             { (void)w; return 0; }
static int  h_hw_str(int w, char *b, int m) { (void)w; (void)b; (void)m; return -1; }

/* ---- v2.6..v2.10: not needed by go.c, but the table is 57 slots wide ---- */
static void *h_brk(void *addr) { (void)addr; return 0; }
static int  h_display_mode(int m)             { (void)m; return -8; }
static int  h_display_info(int w)             { (void)w; return -8; }
static void h_set_style(int fg, int bg, int a) { (void)fg; (void)bg; (void)a; }
static void h_set_style_rgb(uint32_t fg, uint32_t bg, int a) { (void)fg; (void)bg; (void)a; }
static int  h_set_palette(int i, uint32_t rgb) { (void)i; (void)rgb; return 0; }
static uint32_t h_palette(int i)              { (void)i; return 0; }
static void *h_mmap(void *a, uint32_t l, int pr, int fl) {
    (void)a; (void)l; (void)pr; (void)fl;
    return 0;
}
static int  h_munmap(void *a, uint32_t l)      { (void)a; (void)l; return -22; }

/* Designated initializers: the slot order comes from the header, so a
 * reordering there is a compile error here rather than a silent mix-up. */
static catos_api_t g_api = {
    .write        = h_write,
    .putc         = h_putc,
    .cls          = h_cls,
    .readline     = h_readline,
    .time_str     = h_time_str,
    .fs_ready     = h_fs_ready,
    .ls           = h_ls,
    .touch        = h_touch,
    .mkdir        = h_mkdir,
    .cat          = h_cat,
    .save         = h_save,
    .alloc        = h_alloc,
    .free         = h_free,
    .exit         = h_exit,
    .print_int    = h_print_int,
    .print_hex    = h_print_hex,
    .set_fg       = h_set_fg,
    .delay        = h_delay,
    .atoi         = h_atoi,
    .strlen       = h_strlen,
    .strcmp       = h_strcmp,
    .strcpy       = h_strcpy,
    .getch        = h_getch,
    .file_exists  = h_file_exists,
    .fsize        = h_fsize,
    .net_init     = h_net_init,
    .net_up       = h_net_up,
    .http_get     = h_http_get,
    .net_dns      = h_net_dns,
    .net_udp_send = h_net_udp_send,
    .net_udp_recv = h_net_udp_recv,
    .net_ping     = h_net_ping,
    .net_ip       = h_net_ip,
    .getuid       = h_getuid,
    .gettid       = h_gettid,
    .kill         = h_kill,
    .yield        = h_yield,
    .hw_num       = h_hw_num,
    .hw_str       = h_hw_str,
    .file_read    = h_file_read,
    .file_write   = h_file_write,
    .file_delete  = h_file_delete,
    .pipe_create  = h_pipe_create,
    .pipe_write   = h_pipe_write,
    .pipe_read    = h_pipe_read,
    .pipe_close   = h_pipe_close,
    .spawn        = h_spawn,
    .wait         = h_wait,
    .brk          = h_brk,
    .display_mode = h_display_mode,
    .display_info = h_display_info,
    .set_style    = h_set_style,
    .set_style_rgb = h_set_style_rgb,
    .set_palette  = h_set_palette,
    .palette      = h_palette,
    .mmap         = h_mmap,
    .munmap       = h_munmap,
};

int main(int argc, char **argv) {
    __ce_api = &g_api;
    return ce_main(argc, argv);
}
