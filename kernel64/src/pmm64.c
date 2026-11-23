/* pmm64.c - P0.3: physical frame allocator, 1 bit / 4 KiB, 0-4 GiB */
#include "k64.h"

#define FRAMES (PMM_CAP / PMM_FRAME)          /* 1048576 frames */
#define BITS   (FRAMES / 8)                   /* 131072 bytes   */

static u8 bitmap[BITS];
static u64 total, used;

static void set_used(u64 f) { bitmap[f >> 3] |= (u8)(1u << (f & 7)); }
static void set_free(u64 f) { bitmap[f >> 3] &= (u8)~(1u << (f & 7)); }
static int  is_used(u64 f)  { return (bitmap[f >> 3] >> (f & 7)) & 1; }

static void range_free(u64 addr, u64 len)
{
    u64 first = addr / PMM_FRAME;
    u64 last  = (addr + len) / PMM_FRAME;
    for (u64 f = first; f < last && f < FRAMES; f++) set_free(f);
}

static void range_used(u64 addr, u64 len)
{
    u64 first = addr / PMM_FRAME;
    u64 last  = (addr + len + PMM_FRAME - 1) / PMM_FRAME;
    for (u64 f = first; f < last && f < FRAMES; f++) set_used(f);
}

void pmm_init(struct multiboot *mb, u64 kernel_end)
{
    total = FRAMES;
    for (u64 i = 0; i < BITS; i++) bitmap[i] = 0xFF;   /* start with all used */

    if (mb && (mb->flags & (1u << 6)) && mb->mmap_addr) {
        u64 p = mb->mmap_addr;
        u64 end = p + mb->mmap_length;
        while (p < end) {
            struct mmap_entry *e = (struct mmap_entry *)p;
            if (e->type == 1 && e->addr < PMM_CAP)
                range_free(e->addr, e->len);
            p += e->size + 4;
        }
    } else {
        range_free(0x100000, 0x7F00000);               /* fallback: 128 MiB */
    }

    range_used(0, 0x100000);                           /* BIOS/low memory */
    range_used(KPHYS0, kernel_end - KPHYS0 + 0x1000);  /* kernel image      */
    if (mb) {
        range_used((u64)mb, sizeof(struct multiboot));
        if (mb->mmap_addr) range_used(mb->mmap_addr, mb->mmap_length);
    }

    used = 0;
    for (u64 f = 0; f < FRAMES; f++) if (is_used(f)) used++;
}

u64 pmm_alloc(void)
{
    for (u64 f = 0; f < FRAMES; f++) {
        if (!is_used(f)) { set_used(f); used++; return f * PMM_FRAME; }
    }
    return 0;
}

/* one 2 MiB-aligned run of 512 frames (used for the ring3 user windows) */
u64 pmm_alloc_2m(void)
{
    for (u64 f = 0; f + 512 <= FRAMES; f += 512) {
        int ok = 1;
        for (u64 k = 0; k < 512; k++) if (is_used(f + k)) { ok = 0; break; }
        if (ok) {
            for (u64 k = 0; k < 512; k++) { set_used(f + k); used++; }
            return f * PMM_FRAME;
        }
    }
    return 0;
}

void pmm_free(u64 pa)
{
    u64 f = pa / PMM_FRAME;
    if (f < FRAMES && is_used(f)) { set_free(f); used--; }
}

u64 pmm_total(void) { return total; }
u64 pmm_used(void)  { return used; }
