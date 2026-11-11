/* vprobe.c -- ring3 regression probe for user-pointer syscall validation.
 *
 * Issue #78 (2026-11-10): several syscalls called
 *     validate_uaddr(t, buf, 1)
 * and then wrote (or read) up to 16 KiB through the raw user pointer.
 * validate_uaddr() faults in demand-paged VMAs page by page, but with len=1
 * only the FIRST page was mapped.  A ring0 store into the second page took
 * an unrecoverable page fault and isr_exception() halted the whole machine
 * -- any ring3 CE program could kill the kernel.
 *
 * This probe deliberately uses a heap allocation whose first page is mapped
 * and whose second page is still not-present:
 *
 *     p = api->alloc(16)          -> 0x02100008 (heap page 0 mapped)
 *     base = p & ~0xfff           -> 0x02100000
 *     base + 4096                 -> 0x02101000 (NOT mapped)
 *
 * Every case makes the kernel cross that boundary.  It must either fault
 * the pages in or reject the range; it must never take a ring0 fault.
 * Verdict lines consumed by tests/vprobe_regress.py:
 *     VPROBE <case> PASS / VPROBE <case> FAIL
 *
 * Built with -std=c99 -mno-80387, no libc.  Keep comments plain ASCII.
 */
#include "ce_abi.h"

catos_api_t *__ce_api __attribute__((section(".ce_api"))) = (catos_api_t *)0;

#define PAGE 4096u

static void puts_(const char *s) { __ce_api->write(s); }

/* Return the base of the one mapped heap page behind a 16-byte allocation,
 * or 0 when the heap is unavailable. */
static unsigned char *heap_page(void) {
    unsigned char *p = (unsigned char *)__ce_api->alloc(16);
    if (!p) return 0;
    return (unsigned char *)(((unsigned int)p) & ~(PAGE - 1));
}

/* SYS_FILE_READ: up to 16 KiB copied into a buffer with one mapped page. */
static int case_file_read(void) {
    unsigned char *base = heap_page();
    if (!base) return 0;
    char *buf = (char *)(base + 8);          /* exactly alloc(16) payload */
    int n = __ce_api->file_read("/bin/cc.ce", buf, 16384, 0);
    if (n <= 0) return 0;
    /* The kernel must have copied real bytes, not just claimed success. */
    return (n > 4096 && buf[0] == 'C' && buf[1] == 'E') ? 1 : 0;
}

/* SYS_CAT: 4095 bytes from a large built-in binary, also crosses a page. */
static int case_cat(void) {
    unsigned char *base = heap_page();
    if (!base) return 0;
    char *buf = (char *)(base + 8);
    int n = __ce_api->cat("/bin/cc.ce", buf, 4096);
    return (n > 4096 - 8 && buf[0] == 'C' && buf[1] == 'E') ? 1 : 0;
}

/* SYS_HW_STR: a NUL-terminated string written at the very end of page 0. */
static int case_hw_str(void) {
    unsigned char *base = heap_page();
    if (!base) return 0;
    char *buf = (char *)(base + PAGE - 4);
    int n = __ce_api->hw_str(2 /* HW_CPU_BRAND_S */, buf, 4096);
    return (n > 0 && buf[0] != 0) ? 1 : 0;
}

/* SYS_STRLEN / STRCMP / ATOI: strings that run off the end of page 0. */
static int case_str(void) {
    unsigned char *base = heap_page();
    if (!base) return 0;
    char *s = (char *)(base + PAGE - 8);
    int i;
    for (i = 0; i < 8; i++) s[i] = 'A';      /* no NUL on page 0 */
    if (__ce_api->strlen(s) != 8) return 0;
    if (__ce_api->strcmp(s, "AAAAAAAA") != 0) return 0;
    {
        char *q = (char *)(base + PAGE - 5);
        q[0] = '1'; q[1] = '2'; q[2] = '3'; q[3] = '4'; q[4] = '5';
        if (__ce_api->atoi(q) != 12345) return 0;
    }
    return 1;
}

/* SYS_STRCPY: a copy that starts 4 bytes before the page boundary. */
static int case_strcpy(void) {
    unsigned char *base = heap_page();
    if (!base) return 0;
    char *d = (char *)(base + PAGE - 4);
    __ce_api->strcpy(d, "ABCDEFGH");
    return (d[0] == 'A' && d[7] == 'H' && d[8] == 0) ? 1 : 0;
}

/* SYS_WRITE: a NUL-terminated string whose terminator sits in page 1.
 * The kernel must snapshot through copy_from_user() and must not fault in
 * the page after the NUL (or crash while reading it). */
static int case_write(void) {
    unsigned char *base = heap_page();
    if (!base) return 0;
    char *s = (char *)(base + PAGE - 6);
    s[0] = 'A'; s[1] = 'B'; s[2] = 'C'; s[3] = 'D'; s[4] = 'E'; s[5] = 0;
    puts_("VPROBE-WR ");
    __ce_api->write(s);
    puts_("\n");
    return 1;                       /* surviving is the assertion */
}

/* SYS_FILE_WRITE: an 8 KiB user buffer crossing into page 1, taken by the
 * kernel through a chunked copy_from_user() snapshot.  Read the head and
 * tail back to prove the bytes actually reached the file. */
static int case_file_write(void) {
    unsigned char *base = heap_page();
    if (!base) return 0;
    char *buf = (char *)(base + PAGE - 8);
    char chk[256];
    int i;
    for (i = 0; i < 8192; i++) buf[i] = (char)('a' + (i % 26));
    if (__ce_api->file_write("/VPROBE.DAT", buf, 8192, 0) != 0) return 0;
    if (__ce_api->file_read("/VPROBE.DAT", chk, 256, 0) != 256) return 0;
    for (i = 0; i < 256; i++) if (chk[i] != buf[i]) return 0;
    if (__ce_api->file_read("/VPROBE.DAT", chk, 256, 7936) != 256) return 0;
    for (i = 0; i < 256; i++) if (chk[i] != buf[7936 + i]) return 0;
    __ce_api->file_delete("/VPROBE.DAT");
    return 1;
}

/* SYS_PIPE_WRITE: the pipe loop may sleep (task switch => different CR3),
 * so a raw user pointer is especially dangerous here.  Use 2 KiB, small
 * enough that the single writer never has to block on a full pipe. */
static int case_pipe_write(void) {
    unsigned char *base = heap_page();
    if (!base) return 0;
    char *buf = (char *)(base + PAGE - 8);
    char chk[256];
    int i, h;
    for (i = 0; i < 2048; i++) buf[i] = (char)('A' + (i % 26));
    h = __ce_api->pipe_create();
    if (h < 0) return 0;
    if (__ce_api->pipe_write(h, buf, 2048) != 0) return 0;
    if (__ce_api->pipe_read(h, chk, 256) != 256) return 0;
    for (i = 0; i < 256; i++) if (chk[i] != buf[i]) return 0;
    __ce_api->pipe_close(h);
    return 1;
}

struct probe_case {
    const char *name;
    int (*fn)(void);
};

static const struct probe_case CASES[] = {
    { "fr",     case_file_read },
    { "cat",    case_cat },
    { "hw",     case_hw_str },
    { "str",    case_str },
    { "strcpy", case_strcpy },
    { "wr",     case_write },
    { "fw",     case_file_write },
    { "pw",     case_pipe_write },
};

__attribute__((section(".text.ce_entry")))
int ce_main(int argc, char **argv) {
    catos_api_t *api = __ce_api;
    unsigned int i;
    if (argc < 2 || !argv || !argv[1]) {
        puts_("VPROBE: usage: vprobe.ce <case>\n");
        return 0;
    }
    for (i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
        int ok;
        if (api->strcmp(CASES[i].name, argv[1]) != 0) continue;
        ok = CASES[i].fn();
        puts_("VPROBE "); puts_(CASES[i].name);
        if (ok) puts_(" PASS\n"); else puts_(" FAIL\n");
        return ok ? 0 : 1;
    }
    puts_("VPROBE: unknown case\n");
    return 0;
}
