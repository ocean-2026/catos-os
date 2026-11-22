/* gfxdesk.c -- PROTOTYPE (v2.73): gfx24 true-colour desktop environment.
 *
 * A proof-of-concept CDE desktop that runs entirely in ring3 on the
 * existing gfx24 drawing ABI (gfx_rect/gfx_blit, no kernel changes).
 * Uses the genuine IBM VGA 8x16 font embedded in the CE image and draws
 * crisp 24bpp windows/icons/taskbar into the 800x600 LFB.
 *
 * This is deliberately a SEPARATE app from desktop.c so the text backend
 * (and its 10/10 probe) stays untouched until this is proven on real HW.
 *
 * Controls: mouse click icon/title to focus; [Start] menu; q/Esc to exit.
 *
 * v2.74: the earlier "kernel gfx-mode sleep/keypress stall" was disproved
 * by COM1 scheduler breadcrumbs (tests/gfxsleep_diag.py).  The CE loop,
 * its api->delay()/task_sleep_ticks() wakeups, pollkey() and the 'q' key
 * all work correctly in gfx24.  The old XFAIL came from the PROBE: on
 * exit this app switches to VBE (mode 2, not TEXT -- switching to TEXT
 * requires user consent), so its "exited" line lands in the VBE backend
 * and a text-buffer read cannot see it.  gfxdesk_probe now verifies exit
 * via a gfx pixel marker.  See docs/CDE-模块化设计.md §10.5.
 */
#include "ce_abi.h"
#include "ce_app.h"

#include "../src/font_ascii.inc"   /* static const unsigned char [95][16] */

#define SCALE 2
#define CW (8 * SCALE)
#define CH (16 * SCALE)
#define COLS 24
#define ROWS 6
#define SW (COLS * CW)             /* 384 */
#define SH (ROWS * CH)             /* 192 */

static unsigned char fb[SW * SH * 3];   /* 216 KB, under CE_MAX_BSS */
static gfx_rect_t rect;
static gfx_blit_t blit;

/* palette (0x00RRGGBB) */
#define P_BG     0x0a0e14u
#define P_TITLE  0x1f3a5fu
#define P_ACCENT 0x357eceu
#define P_WHITE  0xe0e6edu
#define P_GREEN  0x7ecec8u
#define P_YELLOW 0xf0c674u
#define P_RED    0xff9b9bu
#define P_DIM    0x8894a8u
#define P_SEL    0x2a5a7au

static void px(int x, int y, unsigned r, unsigned g, unsigned b) {
    if (x < 0 || x >= SW || y < 0 || y >= SH) return;
    unsigned char *p = fb + (y * SW + x) * 3;
    p[0] = r; p[1] = g; p[2] = b;
}
static void box(int x, int y, int w, int h, uint32_t rgb) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) px(x + i, y + j,
            (rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
}
static void glyph(char ch, int col, int row, uint32_t rgb) {
    unsigned char c = (unsigned char)ch;
    if (c < 0x20 || c > 0x7e) c = '?';
    const unsigned char *rows = font_ascii[c - 0x20];
    int ox = col * CW, oy = row * CH;
    for (int j = 0; j < 16; j++) {
        unsigned char bits = rows[j];
        for (int i = 0; i < 8; i++)
            if (bits & (0x80 >> i))
                box(ox + i * SCALE, oy + j * SCALE, SCALE, SCALE, rgb);
    }
}
static void text(int col, int row, const char *s, uint32_t rgb) {
    while (*s && col < COLS) glyph(*s++, col++, row, rgb);
}
static void screen_rect(catos_api_t *api, int x, int y, int w, int h, uint32_t rgb) {
    rect.x = x; rect.y = y; rect.w = w; rect.h = h; rect.rgb = rgb;
    api->gfx_rect(&rect);
}
/* flush the offscreen window buffer at a screen position */
static void flush(catos_api_t *api, int x, int y) {
    blit.src = fb; blit.x = x; blit.y = y; blit.w = SW; blit.h = SH;
    api->gfx_blit(&blit);
}

/* --- desktop model ---------------------------------------------------- */
static const char *const term_body[] = {
    "catos:/# ls -l", "  gfxdesk.ce  hello", "  bc.ce  pkgs.txt",
    "catos:/# echo hi", "hi",
};
static const char *const files_body[] = {
    "/bin/", "  gfxdesk.ce", "  gfxtext.ce", "/pkg/", "  desktop.catpkg",
};
static const char *const about_body[] = {
    "CatOS Desktop", "gfx24 true colour", "CE-side window mgr",
    "kernel: primitives", "no ABI change",
};
typedef struct {
    const char *title;
    int x, y;                 /* screen pixel origin of window */
    const char *const *lines;
    int nlines;
} win_t;
static win_t wins[] = {
    {"Terminal", 60, 60,  term_body,  5},
    {"Files",   420, 96,  files_body, 5},
    {"About",   220, 330, about_body, 5},
};
#define MAXW 3
static const char *icons[] = {"Terminal", "Files", "About"};
#define NICONS 3
static int focused = 0;
static int start_open = 0;

/* draw one window into the shared offscreen buffer (cleared by caller) */
static void draw_win(int idx) {
    const win_t *w = &wins[idx];
    int is_focus = (idx == focused);
    box(0, 0, SW, SH, P_BG);
    box(0, 0, SW, CH + 4, is_focus ? P_TITLE : 0x16232f);
    box(0, CH + 4, SW, 2, is_focus ? P_ACCENT : 0x1d3547);
    text(1, 0, w->title, P_WHITE);
    for (int i = 0; i < w->nlines && i < ROWS - 2; i++)
        text(1, i + 2, w->lines[i], i % 2 ? P_GREEN : P_WHITE);
}

static void compose(catos_api_t *api) {
    /* whole-screen background + taskbar (cheap rects, no big buffer) */
    screen_rect(api, 0, 0, 800, 600, P_BG);
    screen_rect(api, 0, 576, 800, 24, P_TITLE);
    screen_rect(api, 0, 574, 800, 2, P_ACCENT);
    /* icon labels are drawn in small buffers via blit below */
    /* [Start] + hints rendered into fb then blitted to taskbar */
    box(0, 0, SW, SH, P_TITLE);
    text(0, 0, start_open ? "[Start*]" : "[Start]", P_WHITE);
    text(9, 0, "Desktop  click icon/title  q/Esc=quit", P_DIM);
    blit.src = fb; blit.x = 4; blit.y = 578; blit.w = SW; blit.h = CH;
    api->gfx_blit(&blit);

    /* icons: label buffer per icon, at left column */
    for (int i = 0; i < NICONS; i++) {
        box(0, 0, SW, CH, i == focused ? P_SEL : P_BG);
        box(0, 0, CW / 2, CH, P_ACCENT);          /* icon glyph square */
        text(1, 0, icons[i], i == focused ? P_YELLOW : P_WHITE);
        blit.src = fb; blit.x = 24; blit.y = 40 + i * 56;
        blit.w = CW * 12; blit.h = CH;
        api->gfx_blit(&blit);
    }

    /* the focused window */
    draw_win(focused);
    flush(api, wins[focused].x, wins[focused].y);

    /* Start menu popup */
    if (start_open) {
        box(0, 0, SW, SH, 0x16232f);
        text(0, 0, "[q] quit desktop", P_WHITE);
        text(0, 1, "[1] Terminal      ", P_DIM);
        text(0, 2, "[2] Files         ", P_DIM);
        text(0, 3, "[3] About         ", P_DIM);
        blit.src = fb; blit.x = 4; blit.y = 500; blit.w = SW; blit.h = CH * 4;
        api->gfx_blit(&blit);
    }
}

static void cursor(catos_api_t *api, int x, int y, int visible) {
    if (!visible) return;
    screen_rect(api, x, y, 12, 2, P_WHITE);
    screen_rect(api, x, y + 2, 2, 12, P_WHITE);
    screen_rect(api, x + 2, y + 10, 2, 2, P_WHITE);
}

__attribute__((section(".text.ce_entry")))
int ce_main(int argc, char **argv) {
    catos_api_t *api = CE_API();
    int pb_ = 0;
    (void)argc; (void)argv;

    if (!api->gfx_info || !api->gfx_rect || !api->gfx_blit || !api->display_mode ||
        !api->mouse_x || !api->mouse_y || !api->mouse_buttons) {
        api->write("gfxdesk: kernel lacks gfx/mouse ABI\n");
        return 1;
    }
    if (api->gfx_info(GFX_INFO_ACTIVE) != 1) {
        api->display_mode(3);   /* switching INTO gfx is free */
        if (api->gfx_info(GFX_INFO_ACTIVE) != 1) {
            api->write("gfxdesk: mode not vbe24\n");
            return 1;
        }
    }

    compose(api);
    for (;;) {
        int x = api->mouse_x(), y = api->mouse_y(), b = api->mouse_buttons();
        int k = api->pollkey ? api->pollkey() : -1;
        int changed = 0;

        if (k == 'q' || k == 0x1B) break;

        if ((b & 1) && !pb_) {   /* left-button press edge */
            if (y >= 576 && x <= 64) { start_open = !start_open; changed = 1; }
            else if (start_open && y >= 500 && y < 500 + CH * 4) {
                if (y < 500 + CH) { focused = 0; changed = 1; }
                else if (y < 500 + CH * 2) { focused = 1; changed = 1; }
                else if (y < 500 + CH * 3) { focused = 2; changed = 1; }
                start_open = 0;
            }
            else if (x >= 24 && x < 24 + CW * 12 && y >= 40 && y < 40 + NICONS * 56) {
                int i = (y - 40) / 56;
                if (i >= 0 && i < NICONS) { focused = i; changed = 1; }
            }
            else {
                for (int i = 0; i < MAXW; i++) {
                    win_t *w = &wins[i];
                    if (y >= w->y && y < w->y + CH + 4 &&
                        x >= w->x && x < w->x + SW) {
                        focused = i; changed = 1; break;
                    }
                }
            }
        }
        if (k == '1') { focused = 0; changed = 1; }
        if (k == '2') { focused = 1; changed = 1; }
        if (k == '3') { focused = 2; changed = 1; }

        if (changed) compose(api);
        if (api->delay) api->delay(30);
    }

    /* Request TEXT on exit.  The kernel prompts for consent (press Y);
     * once granted this marker lands in the TEXT buffer where the shell
     * and text-buffer probes can see it.  Until then the screen stays in
     * gfx.  (Switching to TEXT silently is deliberately not allowed.) */
    api->display_mode(0);       /* CON_MODE_TEXT -- consent prompt, press Y */
    api->write("gfxdesk: exited to text shell\n");
    return 0;
}
