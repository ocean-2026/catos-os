/*
 * LEGACY REFERENCE ONLY, NOT SHIPPED.
 *
 * This file is retained in place as a learning reference for the old
 * user-space CE desktop prototype. It is deliberately NOT listed in
 * build/apps.list or build/pkg.list and is no longer packaged into the
 * CatOS image. The current block-based declarative desktop lives in the
 * kernel-side gui/ subsystem; do not revive this CE app in shipped builds.
 */
#include "ce_app.h"

/*
 * desk.c -- CatOS detachable desktop environment (P1, v2.69).
 *
 * This is a plain CE application: ALL desktop semantics (windows, focus,
 * taskbar, icon hit-testing, event loop) live here in user space.  The
 * kernel only provides generic primitives -- new CE ABI slots 66/67/68
 * (mouse_x/y/buttons) plus the existing gotoxy(65)/write/cls/pollkey.
 *
 * It runs in the text backend (80x25) using ASCII window chrome so it is
 * testable on both QEMU and VMware via the VGA text buffer.  Graphical
 * gfx_text rendering is P1b.  The whole thing is packaged as desktop.catpkg
 * and reaches /bin/desktop.ce only after `pkg install desktop`; `pkg remove
 * desktop` detaches it and the kernel is unchanged.
 *
 * Controls:
 *   move mouse over an icon + left click  -> open/focus a window
 *   click a window title bar              -> focus it
 *   click [Start] then (q)uit             -> exit back to text shell
 *   Esc / 'q' key                         -> exit
 */
#define COLS 80
#define ROWS 25
#define MAXW 3

typedef struct {
    int id;
    const char *title;
    int x, y, w, h;
    const char *const *lines;
    int nlines;
} win_t;

static const char *const term_body[] = {
    "catos:/# ls -l", "  desk.ce   hello.ce", "  bc.ce     pkgs.txt",
};
static const char *const files_body[] = {
    "/bin/", "  desk.ce", "  gfxdemo.ce", "/pkg/", "  desktop.catpkg",
};
static const char *const about_body[] = {
    "CatOS Desktop 1.0", "CE-side window manager", "kernel: primitives only",
};

static win_t wins[MAXW] = {
    {1, "Terminal",  6,  4, 30, 8, term_body,  3},
    {2, "Files",    40,  6, 32, 9, files_body, 5},
    {3, "About",    18, 13, 34, 8, about_body, 3},
};

static const char *icons[] = {"Terminal", "Files", "About"};
#define NICONS 3

static void put(catos_api_t *api, int x, int y, const char *s) {
    if (api->gotoxy(x, y) != 0) return;
    api->write(s);
}

static void draw_win(catos_api_t *api, const win_t *w, int focused) {
    int i;
    char bar[40];
    for (i = 0; i < w->w; i++) bar[i] = focused ? '=' : '-';
    bar[w->w] = 0;
    put(api, w->x, w->y, "+");
    put(api, w->x + 1, w->y, w->title);
    put(api, w->x + 1 + (int)api->strlen(w->title), w->y, bar);
    for (i = 0; i < w->nlines && i < w->h - 2; i++)
        put(api, w->x + 2, w->y + 1 + i, w->lines[i]);
    put(api, w->x, w->y + w->h - 1, bar);
}

static void compose(catos_api_t *api, int focused, int start_open) {
    int i;
    api->cls();
    /* desktop icons down the left column */
    for (i = 0; i < NICONS; i++) {
        put(api, 2, 2 + i * 2, "[");
        put(api, 4, 2 + i * 2, icons[i]);
        put(api, 4 + (int)api->strlen(icons[i]), 2 + i * 2, "]");
    }
    for (i = 0; i < MAXW; i++)
        draw_win(api, &wins[i], wins[i].id == focused);
    /* taskbar */
    put(api, 0, ROWS - 1, "[Start]  Desktop   (mouse: click icon/title, "
                          "Start>q=quit)  focus=");
    {
        char n[2]; n[0] = (char)('0' + focused); n[1] = 0;
        put(api, 70, ROWS - 1, n);
    }
    if (start_open)
        put(api, 1, ROWS - 2, "[q] quit desktop   [r] Files   [t] Terminal");
}

__attribute__((section(".text.ce_entry")))
int ce_main(int argc, char **argv) {
    catos_api_t *api = CE_API();
    int focused = 1, start_open = 0;
    int lastx = -1, lasty = -1, lastb = 0;
    (void)argc; (void)argv;

    if (!api->mouse_x || !api->mouse_y || !api->mouse_buttons) {
        api->write("desktop: kernel lacks mouse ABI (slots 66-68)\n");
        return 1;
    }

    compose(api, focused, start_open);
    for (;;) {
        int x = api->mouse_x(), y = api->mouse_y(), b = api->mouse_buttons();
        int k = api->pollkey ? api->pollkey() : -1;
        int changed = 0;

        if (k == 'q' || k == 0x1B || (start_open && k == 'q')) break;
        if (k == 'r') { focused = 2; changed = 1; }
        if (k == 't') { focused = 1; changed = 1; }

        /* press edge (left button just went down) */
        if ((b & 1) && !lastb && x != lastx - 100) {
            int i;
            /* Start button: bottom-left */
            if (y == ROWS - 1 && x >= 0 && x <= 6) {
                start_open = !start_open; changed = 1;
            }
            /* Start menu "quit" hint at row ROWS-2, x 1..10 */
            else if (start_open && y == ROWS - 2 && x >= 1 && x <= 10) break;
            /* icons */
            else if (y >= 2 && y <= 8 && x >= 2 && x <= 16) {
                for (i = 0; i < NICONS; i++)
                    if (y == 2 + i * 2) { focused = i + 1; changed = 1; }
            }
            /* window title bars -> focus (windows drawn back-to-front) */
            else {
                for (i = MAXW - 1; i >= 0; i--) {
                    win_t *w = &wins[i];
                    if (y == w->y && x >= w->x && x <= w->x + w->w) {
                        focused = w->id; changed = 1; break;
                    }
                }
            }
        }
        lastx = x; lasty = y; lastb = b;
        if (changed) compose(api, focused, start_open);
        if (api->delay) api->delay(30);
    }
    api->cls();
    api->write("desktop: exited to text shell\n");
    return 0;
}
