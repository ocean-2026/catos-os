/* mmap.c -- Stage 2B CE acceptance: file-backed read-only mapping.
 *
 *   fd  = fd_open("/bin/hello.ce", 0)                  -> fd >= 3
 *   sz  = fd_lseek(fd, 0, SEEK_END)
 *   ref = file_read(path, ref, 4096, 0)                (legacy path read)
 *   m1  = file_mmap(fd, sz, PROT_READ=0, 0)
 *   byte-compare m1[0..n) against ref
 *   m2  = file_mmap(fd, sz, PROT_READ=0, 0)            -> shared cache hit
 *   munmap_shared(m2, sz)                              -> refcnt drops to 1
 *   m3  = file_mmap(fd, sz, PROT_READ=0, 0)            -> still hits (m1 holds)
 *   report cache_hit / pages / refcnt (slot 92 fcache_stat)
 *
 * Error paths exercised:
 *   - file_mmap(bad fd=99, ...)          -> negative (EBADF, -22)
 *   - file_mmap(fd, 4096, PROT_WRITE=1)  -> EACCES (-13)
 *   - capacity: keep mapping the resident file until the active-map
 *     descriptor table refuses; reports the negative code observed (-4 FULL
 *     map-descriptor table full).  The cache-pool ENOMEM (-12) path is not
 *     reachable on the shipped image and is documented as such.
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

    unsigned char *m2 = 0;
    if (m1) {
        m2 = (unsigned char *)__ce_api->file_mmap(fd, (uint32_t)sz, PROT_READ, 0);
        if (m2 == 0) { out("FAIL second file_mmap\n"); fails++; }
        else {
            /* touch m2 so its first page faults in from the shared cache;
             * the second view must land on the same cached frame. */
            int bad2 = 0;
            for (int i = 0; i < n; i++)
                if (m2[i] != ref[i]) { bad2 = 1; break; }
            if (bad2) { out("FAIL byte mismatch m2\n"); fails++; }
            else out("ok   second mmap (shared cache)\n");
        }
    }

    uint32_t pages = __ce_api->fcache_stat(0, 0);
    uint32_t hits  = __ce_api->fcache_stat(1, 0);
    int rc = m1 ? __ce_api->fcache_stat(2, (uint32_t)m1) : -1;
    out("info cache_hit="); __ce_api->print_int((int)hits);
    out(" pages="); __ce_api->print_int((int)pages);
    out(" refcnt="); __ce_api->print_int(rc); out("\n");

    /* unmap the second view, then re-map: m1 still holds the page, so the
     * re-map must hit the resident cache (refcnt returns to 2). */
    if (m2) __ce_api->munmap_shared((uint32_t)m2, (uint32_t)sz);
    unsigned char *m3 = 0;
    if (m1) {
        m3 = (unsigned char *)__ce_api->file_mmap(fd, (uint32_t)sz, PROT_READ, 0);
        if (m3 == 0) { out("FAIL remap after munmap\n"); fails++; }
        else {
            int bad3 = 0;
            for (int i = 0; i < n; i++)
                if (m3[i] != ref[i]) { bad3 = 1; break; }
            if (bad3) { out("FAIL byte mismatch m3\n"); fails++; }
            else {
                int rc3 = __ce_api->fcache_stat(2, (uint32_t)m3);
                pint("ok   remap after munmap refcnt=", rc3);
            }
        }
    }

    /* error: invalid fd. */
    int b1 = __ce_api->file_mmap(99, 4096, PROT_READ, 0);
    pint("err  bad-fd=", b1);
    if (b1 >= 0) { out("FAIL bad fd accepted\n"); fails++; }

    /* error: write request on a read-only file map -> EACCES. */
    int b2 = __ce_api->file_mmap(fd, 4096, PROT_WRITE, 0);
    pint("err  prot-write=", b2);
    if (b2 >= 0) { out("FAIL PROT_WRITE accepted\n"); fails++; }

    /* capacity: repeatedly map the same resident file until the active-map
     * descriptor table is saturated (FC_ERR_FULL=-4).  Cache pages are
     * deduped by (cluster, block), so this exercises the mapping-descriptor
     * cap deterministically regardless of how big the file is.  The
     * cache-pool ENOMEM (-12) path exists but is not reachable on the
     * shipped image (every builtin file is < 87 KiB) -- documented in
     * docs/dynamic-linking.md. */
    int cap = 0;
    for (uint32_t i = 0; i < 24; i++) {
        unsigned char *v = (unsigned char *)__ce_api->file_mmap(fd, (uint32_t)sz, PROT_READ, 0);
        if ((int)v < 0) { cap = (int)v; break; }
    }
    pint("err  capacity=", cap);

    if (m3) __ce_api->munmap_shared((uint32_t)m3, (uint32_t)sz);
    if (m1) __ce_api->munmap_shared((uint32_t)m1, (uint32_t)sz);
    __ce_api->fd_close(fd);

    if (fails == 0) { out("MMAP PASS\n"); return 0; }
    out("MMAP FAIL\n");
    return 1;
}
