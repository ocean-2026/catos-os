/*
 * LEGACY REFERENCE ONLY, NOT SHIPPED.
 *
 * 旧命令式位图原型（gfx_rect / gfx_blit / display_mode(3) 路径）；块化桌面
 * 只认声明式块树，此路径退役。源码保留作学习参考，已从 build/apps.list
 * 摘除，不再打包进镜像。
 */

/* gfx24.c -- CE probe for the v2.12 24bpp drawing ABI (slots 61..63).
 *
 * This is the ring3 counterpart of the shell's `gfx24` command.  It is
 * the interesting half of the test: a ring3 program must never be able to
 * write VRAM directly (the LFB lives above PDE 15 and is supervisor-only),
 * so it hands a descriptor and a user-space bitmap to the kernel, which
 * validates every page with copy_from_user() before drawing.
 *
 * Verdict lines (consumed by tests/gfx24_regress.py in TEXT mode):
 *   GFX24 CE SKIP (kernel has no drawing ABI)   old kernel
 *   GFX24 CE SKIP (mode not vbe24)              simulator / host
 *   GFX24 CE PASS                               real 24bpp LFB + syscalls
 *   GFX24 CE FAIL ...                           syscall error
 *
 * Pixel contract (asserted from screendump in vbe24 mode):
 *   cyan  60x60 rect at (500,120)  -> (530,150) == (0,255,255)
 *   64x64 RGB ramp at (600,300)    -> (630,332) == (120,128,128)
 */
#include "ce_abi.h"

catos_api_t *__ce_api __attribute__((section(".ce_api"))) = (catos_api_t *)0;

#define BW 64
#define BH 64
static unsigned char bmp[BW * BH * 3];  /* .bss, inside the CE arena */
static gfx_rect_t rect;
static gfx_blit_t blit;

static void puts_(const char *s) { __ce_api->write(s); }
static void putdec(int v)       { __ce_api->print_int(v); }

__attribute__((section(".text.ce_entry")))
int ce_main(void) {
    catos_api_t *api = __ce_api;

    /* Append-only ABI: an older kernel has a shorter table. */
    if (!api->gfx_info || !api->gfx_rect || !api->gfx_blit ||
        !api->display_mode) {
        puts_("GFX24 CE SKIP (kernel has no drawing ABI)\n");
        return 0;
    }
    if (api->gfx_info(GFX_INFO_ACTIVE) != 1) {
        /* Switching INTO a graphics mode is free; only leaving it needs
         * a physical Y.  A host simulator reports inactive and skips. */
        api->display_mode(3);
        if (api->gfx_info(GFX_INFO_ACTIVE) != 1) {
            puts_("GFX24 CE SKIP (mode not vbe24)\n");
            return 0;
        }
    }

    int w     = api->gfx_info(GFX_INFO_WIDTH);
    int h     = api->gfx_info(GFX_INFO_HEIGHT);
    int bpp   = api->gfx_info(GFX_INFO_BPP);
    int pitch = api->gfx_info(GFX_INFO_PITCH);
    puts_("gfx24: "); putdec(w); puts_("x"); putdec(h);
    puts_("x"); putdec(bpp); puts_(" pitch="); putdec(pitch);
    puts_("\n");
    /* The ABI is a LOGICAL 800x600 RGB24 surface.  Pitch is a physical
     * property of the device: 2400 (3 B/px) on Bochs dispi, 3200
     * (4 B/px XRGB) on VMware SVGA II.  Do not hard-code 2400 or the
     * probe would refuse to draw on a perfectly valid framebuffer. */
    if (w != 800 || h != 600 || bpp != 24 || pitch < w * 3) {
        puts_("GFX24 CE FAIL (bad info)\n");
        return 1;
    }

    /* Descriptor-based rect: 5 fields through ONE syscall argument. */
    rect.x = 500; rect.y = 120; rect.w = 60; rect.h = 60;
    rect.rgb = 0x00FFFF;
    if (api->gfx_rect(&rect) != 0) {
        puts_("GFX24 CE FAIL (rect)\n");
        return 1;
    }

    /* 64x64 tightly packed RGB24 ramp.  The kernel copies one row at a
     * time, so a bitmap straddling a page boundary exercises the
     * VMA-backed validation that issue #78 was about. */
    for (int y = 0; y < BH; y++) {
        for (int x = 0; x < BW; x++) {
            unsigned char *p = bmp + (y * BW + x) * 3;
            p[0] = (unsigned char)(x * 4);      /* R */
            p[1] = (unsigned char)(y * 4);      /* G */
            p[2] = 0x80;                        /* B */
        }
    }
    blit.src = bmp; blit.x = 600; blit.y = 300;
    blit.w = BW; blit.h = BH;
    if (api->gfx_blit(&blit) != 0) {
        puts_("GFX24 CE FAIL (blit)\n");
        return 1;
    }

    puts_("GFX24 CE PASS\n");
    return 0;
}
