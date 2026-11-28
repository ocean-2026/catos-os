/* kvheap.c - Stage 3A-1: page-backed virtually-contiguous kernel heap.
 *
 * See include/kvheap.h for the window layout and invariants.  Two allocation
 * paths:
 *   - slab:   <= one frame, rounded up to a power-of-two size class, carved
 *             from a frame shared with sibling blocks of the same class.
 *   - extent: > one frame, one virtual run of N consecutive window slots;
 *             physical frames may be scattered.  Freed extents go on a
 *             free list so a re-allocation of the same size reuses the same
 *             virtual run (selftest same-address check).
 */
#include <stdint.h>
#include "kvheap.h"
#include "paging.h"
#include "serial_log.h"

#define KV_MAGIC_SLAB 0x4B564853u   /* "KVHS" */
#define KV_MAGIC_EXT  0x4B564558u   /* "KVEX" */
#define KV_EXT_CLS    0xFFu
#define KV_HDR        16u

typedef struct kv_hdr {
    uint32_t      magic;
    uint32_t      cls;        /* slab class index; KV_EXT_CLS for extent */
    uint32_t      nframes;    /* extent only */
    struct kv_hdr *next;
} kv_hdr_t;

/* size classes (whole block including the 16-byte header) */
static const uint32_t kv_class[] = { 32, 64, 128, 256, 512, 1024, 2048, 4096 };
#define KV_NCLASS (sizeof(kv_class)/sizeof(kv_class[0]))

/* small static metadata: one free-list head per class + one extent list.
 * This is a fixed small table, not a large .bss array (Stage 3A constraint). */
static kv_hdr_t *kv_free[KV_NCLASS];
static kv_hdr_t *kv_ext_free;

static uint32_t  kv_cursor;          /* next free window page index */
static uint32_t  kv_pages_managed;
static uint32_t  kv_inuse_objs;
static uint32_t  kv_inuse_bytes;

void kvheap_init(void) {
    for (uint32_t c = 0; c < KV_NCLASS; c++) kv_free[c] = 0;
    kv_ext_free = 0;
    kv_cursor = 0;
    kv_pages_managed = 0;
    kv_inuse_objs = 0;
    kv_inuse_bytes = 0;
}

uint32_t kvheap_inuse_bytes(void) { return kv_inuse_bytes; }
uint32_t kvheap_inuse_objs(void)  { return kv_inuse_objs; }

static void *kv_window_slot(uint32_t slot) {
    return (void *)(KVHEAP_BASE + slot * 4096u);
}

/* refill a slab class from a fresh frame; chain every block onto free[c]. */
static int kv_refill(uint32_t c) {
    uint32_t B = kv_class[c];
    uint32_t per = 4096u / B;
    uint32_t pa = frame_alloc();
    if (!pa) return -1;
    uint8_t *f = (uint8_t *)pa;              /* identity-mapped zero-target */
    for (uint32_t i = 0; i < 4096u; i++) f[i] = 0;

    if (kv_cursor >= 8192u) { frame_free(pa); return -1; }
    uint32_t slot = kv_cursor++;
    uint8_t *va = (uint8_t *)kv_window_slot(slot);
    paging_kmap((uint32_t)va, pa, 0);
    kv_pages_managed++;

    for (uint32_t i = 0; i < per; i++) {
        kv_hdr_t *h = (kv_hdr_t *)(va + i * B);
        h->magic = KV_MAGIC_SLAB;
        h->cls   = c;
        h->nframes = 0;
        h->next  = kv_free[c];
        kv_free[c] = h;
    }
    return 0;
}

static void *kv_slab(uint32_t total) {
    uint32_t c;
    for (c = 0; c < KV_NCLASS; c++)
        if (kv_class[c] >= total) break;
    if (c >= KV_NCLASS) return 0;
    if (!kv_free[c] && kv_refill(c) != 0) return 0;
    kv_hdr_t *h = kv_free[c];
    kv_free[c] = h->next;
    h->next = 0;
    kv_inuse_objs++;
    kv_inuse_bytes += kv_class[c];
    return (void *)((uint8_t *)h + KV_HDR);
}

static void *kv_extent(uint32_t total) {
    uint32_t need = (total + 4095u) / 4096u;
    /* first-fit on freed extent runs of the exact frame count */
    kv_hdr_t **pp = &kv_ext_free;
    while (*pp) {
        kv_hdr_t *h = *pp;
        if (h->nframes == need) {
            *pp = h->next;
            h->next = 0;
            kv_inuse_objs++;
            kv_inuse_bytes += need * 4096u;
            return (void *)((uint8_t *)h + KV_HDR);
        }
        pp = &h->next;
    }
    if (kv_cursor + need > 8192u) return 0;
    uint32_t slot0 = kv_cursor;
    kv_cursor += need;
    /* 回滚路径才需要保留各帧 PA；栈上按 need 开 VLA（352 帧 ≈1.4KB，
     * 在 8KB 内核栈内）。旧版固定 pa_tmp[8]，大 extent 成功路径同样越界
     * 写栈，是大块分配崩溃的根因。 */
    uint32_t pa_tmp[need];
    uint32_t got = 0;
    for (uint32_t i = 0; i < need; i++) {
        uint32_t pa = frame_alloc();
        if (!pa) { got = i; pa_tmp[got] = 0; goto fail; }
        pa_tmp[i] = pa;
        uint8_t *f = (uint8_t *)pa;
        for (uint32_t k = 0; k < 4096u; k++) f[k] = 0;
        paging_kmap(KVHEAP_BASE + (slot0 + i) * 4096u, pa, 0);
        kv_pages_managed++;
    }
    {
        kv_hdr_t *h = (kv_hdr_t *)kv_window_slot(slot0);
        h->magic = KV_MAGIC_EXT;
        h->cls   = KV_EXT_CLS;
        h->nframes = need;
        h->next  = 0;
        kv_inuse_objs++;
        kv_inuse_bytes += need * 4096u;
        return (void *)((uint8_t *)h + KV_HDR);
    }
fail:
    for (uint32_t i = 0; i < got; i++) {
        paging_kunmap(KVHEAP_BASE + (slot0 + i) * 4096u);
        frame_free(pa_tmp[i]);
    }
    kv_cursor = slot0;
    return 0;
}

void *kvmalloc(uint32_t size, uint32_t align) {
    (void)align;                              /* payload is 16-aligned */
    if (!size) return 0;
    uint32_t total = size + KV_HDR;
    if (total <= 4096u) return kv_slab(total);
    return kv_extent(total);
}

void kvmfree(void *va) {
    if (!va) return;
    kv_hdr_t *h = (kv_hdr_t *)((uint8_t *)va - KV_HDR);
    if (h->magic == KV_MAGIC_SLAB) {
        uint32_t c = h->cls;
        if (c >= KV_NCLASS) return;
        h->next = kv_free[c];
        kv_free[c] = h;
        kv_inuse_objs--;
        kv_inuse_bytes -= kv_class[c];
    } else if (h->magic == KV_MAGIC_EXT) {
        uint32_t need = h->nframes;
        /* Stage 3A-1 boundary: freed extent runs stay mapped and keep their
         * physical frames attached to the free list (a run cache), so a
         * re-allocation of the same size reuses the same virtual run with its
         * PTEs intact.  Frames are not returned to the pool until shutdown;
         * the cache is bounded by the high-water mark.  Per-frame PA recycling
         * and VA-run compaction are deferred. */
        h->next = kv_ext_free;
        kv_ext_free = h;
        kv_inuse_objs--;
        kv_inuse_bytes -= need * 4096u;
    }
}

/* ---- boot self test ---------------------------------------------------- */

static void ser_dec(uint32_t v) {
    char b[12]; int n = 0;
    if (!v) { ser_putc('0'); return; }
    while (v) { b[n++] = (char)('0' + v % 10u); v /= 10u; }
    while (n) ser_putc(b[--n]);
}

static int fill_check(uint8_t *p, uint32_t n, uint8_t seed) {
    for (uint32_t i = 0; i < n; i++) p[i] = (uint8_t)(seed + i);
    for (uint32_t i = 0; i < n; i++)
        if (p[i] != (uint8_t)(seed + i)) return 0;
    return 1;
}

void kvheap_selftest(void) {
    int ok = 1;
    uint8_t *a = kvmalloc(8192u, 0);
    uint8_t *b = kvmalloc(32768u, 0);
    if (!a || !b) ok = 0;
    if (ok) {
        if (!fill_check(a, 8192u, 0x5Au)) ok = 0;
        if (!fill_check(b, 32768u, 0x33u)) ok = 0;
    }
    kvmfree(a);
    kvmfree(b);
    uint8_t *a2 = kvmalloc(8192u, 0);
    if (a2 != a) ok = 0;                       /* freed extent re-run, same VA */
    kvmfree(a2);

    /* slab same-address round trip */
    void *s = kvmalloc(1024u, 0);
    kvmfree(s);
    void *s2 = kvmalloc(1024u, 0);
    if (s2 != s) ok = 0;
    kvmfree(s2);

    ser_write("kvheap selftest: ");
    ser_write(ok ? "PASS" : "FAIL");
    ser_write(" total="); ser_dec(kv_pages_managed);
    ser_write(" free=");  ser_dec(kv_inuse_objs);
    ser_write(" bytes="); ser_dec(kv_inuse_bytes);
    ser_write("\n");
}
