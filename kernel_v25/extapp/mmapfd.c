/* mmapfd.c -- Stage 2B CE regression: file-backed read-only mapping.
 *
 *   fd  = fd_open("/appdevelop/API.TXT", 0)            -> fd >= 3
 *   sz  = fd_lseek(fd, 0, SEEK_END)
 *   ref = file_read(path, ref, 4096, 0)                (legacy path read)
 *   m1  = file_mmap(fd, sz, PROT_READ=0, 0)
 *   byte-compare m1[0..n) against ref
 *   m2  = file_mmap(fd, sz, PROT_READ=0, 0)            -> shared cache hit
 *   report cache_hit / pages / refcnt (slot 92 fcache_stat)
 *
 * Error paths exercised:
 *   - file_mmap(bad fd=99, ...)        -> negative (EBADF, -22)
 *   - file_mmap(fd, 4096, PROT_WRITE=1) -> EACCES (-13)
 *
 * Prints PASS / FAIL lines; every string ASCII so it runs anywhere.
 */
#include "ce_abi.h"

catos_api_t *__ce_api __attribute__((section(".ce_api"))) = (catos_api_t *)0;

#define PROT_READ  0
#define PROT_WRITE 1
#define SEEK_END   2
#define SEEK_SET   0

static const char *PATH = "/bin/hello.ce";

static void out(const char *s) { __ce_api->write(s); }
static void pint(const char *tag, int v) { out(tag); __ce_api->print_int(v); out("\n"); }

__attribute__((section(".text.ce_entry")))
int ce_main(void) {
    int fails = 0;

    int fd = __ce_api->fd_open(PATH, 0);
    if (fd < 0) { pint("FAIL fd_open=", fd); return 1; }
    pint("ok   fd_open fd=", fd);

    int sz = __ce_api->fd_lseek(fd, 0, SEEK_END);
    if (sz <= 0) { out("FAIL lseek size\n"); fails++; }
    else { pint("ok   file size=", sz); }
    __ce_api->fd_lseek(fd, 0, SEEK_SET);

    unsigned char ref[4096];
    int n = __ce_api->file_read(PATH, (char *)ref, 4096, 0);
    if (n <= 0) { out("FAIL path reference read\n"); fails++; }
    else { pint("ok   path read bytes=", n); }

    unsigned char *m1 = 0;
    if (n > 0) {
        m1 = (unsigned char *)__ce_api->file_mmap(fd, (uint32_t)sz, PROT_READ, 0);
        if (m1 == 0) { out("FAIL file_mmap m1\n"); fails++; }
        else {
            int bad = 0;
            for (int i = 0; i < n; i++)
                if (m1[i] != ref[i]) { bad = 1; break; }
            if (bad) { out("FAIL byte mismatch m1\n"); fails++; }
            else out("ok   mmap byte-match chunk0\n");
        }
    }

    /* second concurrent mapping of the same file: shared pages hit cache. */
    unsigned char *m2 = 0;
    if (m1) {
        m2 = (unsigned char *)__ce_api->file_mmap(fd, (uint32_t)sz, PROT_READ, 0);
        if (m2 == 0) { out("FAIL second file_mmap\n"); fails++; }
        else out("ok   second mmap (shared cache)\n");
    }

    /* cache diagnostics (slot 92). */
    uint32_t pages = __ce_api->fcache_stat(0, 0);
    uint32_t hits  = __ce_api->fcache_stat(1, 0);
    int rc = m1 ? __ce_api->fcache_stat(2, (uint32_t)m1) : -1;
    out("info cache_hit="); __ce_api->print_int((int)hits);
    out(" pages="); __ce_api->print_int((int)pages);
    out(" refcnt="); __ce_api->print_int(rc); out("\n");

    /* error: invalid fd. */
    int b1 = __ce_api->file_mmap(99, 4096, PROT_READ, 0);
    pint("err  bad-fd=", b1);
    if (b1 >= 0) { out("FAIL bad fd accepted\n"); fails++; }

    /* error: write request on a read-only file map -> EACCES. */
    int b2 = __ce_api->file_mmap(fd, 4096, PROT_WRITE, 0);
    pint("err  prot-write=", b2);
    if (b2 >= 0) { out("FAIL PROT_WRITE accepted\n"); fails++; }

    if (m2) __ce_api->munmap_shared((uint32_t)m2, (uint32_t)sz);
    if (m1) __ce_api->munmap_shared((uint32_t)m1, (uint32_t)sz);
    __ce_api->fd_close(fd);

    if (fails == 0) { out("MMAPFD PASS\n"); return 0; }
    out("MMAPFD FAIL\n");
    return 1;
}
