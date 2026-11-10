#ifndef CATOS_GFX24_H
#define CATOS_GFX24_H

/*
 * gfx24.h -- CatOS 24-bit truecolour drawing API (ABI v2.12).
 *
 * This is the FIRST user-visible drawing surface in CatOS.  Before v2.12
 * the framebuffer was strictly supervisor-only and every back-end painted
 * text cells; `display` used to print "user drawing API : NONE".
 *
 * The LFB is STILL supervisor-only: ring3 page directories copy PDEs 0..15
 * only, so a high BAR0 (0xfd000000 on QEMU) is unreachable from user mode.
 * User programs never receive a framebuffer pointer.  Instead they hand a
 * tightly packed RGB24 buffer (or a rectangle description) to the kernel,
 * which validates the user range with copy_from_user()/validate_uaddr()
 * and performs the writes itself.
 *
 * Pixel order in the LFB is B,G,R (three bytes, no padding, no alpha):
 * pitch = 800 * 3 = 2400 bytes, framebuffer = 1,440,000 bytes.  All public
 * RGB values are 0x00RRGGBB, the same encoding as con_set_style_rgb().
 */
#include <stdint.h>

#define GFX24_W         800
#define GFX24_H         600
#define GFX24_BPP       24
#define GFX24_PITCH     (GFX24_W * 3)
#define GFX24_FB_BYTES  (GFX24_PITCH * GFX24_H)

/* gfx_info() selectors (also used by CE syscall slot 61).  Unknown
 * selectors return -8 ENOSYS, matching the rest of the info syscalls. */
enum {
    GFX_INFO_ACTIVE = 0,   /* 1 = 24bpp LFB live, 0 = another mode       */
    GFX_INFO_WIDTH,        /* 800                                       */
    GFX_INFO_HEIGHT,       /* 600                                       */
    GFX_INFO_BPP,          /* 24                                        */
    GFX_INFO_PITCH,        /* 2400 bytes between rows                    */
    GFX_INFO_FB_BYTES,     /* 1440000                                   */
    GFX_INFO_LFB_PHYS      /* PCI BAR0 physical address (supervisor)     */
};

int      gfx24_active(void);
int      gfx24_info(int what);
uint32_t gfx24_lfb_phys(void);

/* All drawing calls clip to the 800x600 surface and are no-ops when the
 * 24bpp back-end is not active.  rgb is 0x00RRGGBB. */
void     gfx24_clear(uint32_t rgb);
void     gfx24_pixel(int x, int y, uint32_t rgb);
void     gfx24_rect(int x, int y, int w, int h, uint32_t rgb);
/* src: w*h*3 tightly packed RGB24 bytes (R,G,B order).  The syscall
 * wrapper copies it into a kernel buffer before calling this. */
void     gfx24_blit(const uint8_t *src, int x, int y, int w, int h);

/* Fixed diagnostic picture used by the shell `gfx24` command and by
 * tests/gfx24_regress.py.  Colour blocks + a 256-step gradient prove that
 * all three 8-bit channels really reach VRAM at 24bpp. */
void     gfx24_demo(void);

#endif /* CATOS_GFX24_H */
