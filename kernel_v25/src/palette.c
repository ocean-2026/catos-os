/*
 * palette.c -- semantic colour roles backed by the hive (v2.67).
 */
#include "palette.h"
#include "hive.h"
#include "render.h"
#include "vga.h"

/* CLUT slots reserved for roles.  32..47 keeps 0..15 (the 16 hardware
 * colours) and 16..31 (theme ramp) untouched, so existing code that uses
 * VGA_* keeps meaning what it did. */
#define PAL_SLOT_BASE 32

static const char *const k_console_paths[PAL_COUNT] = {
    "HKLM\\Software\\CatOS\\Terminal\\Colors\\Background",
    "HKLM\\Software\\CatOS\\Terminal\\Colors\\Foreground",
    "HKLM\\Software\\CatOS\\Terminal\\Colors\\PromptUser",
    "HKLM\\Software\\CatOS\\Terminal\\Colors\\PromptPath",
    "HKLM\\Software\\CatOS\\Terminal\\Colors\\PromptSymbol",
    "HKLM\\Software\\CatOS\\Terminal\\Colors\\Command",
    "HKLM\\Software\\CatOS\\Terminal\\Colors\\Argument",
    "HKLM\\Software\\CatOS\\Terminal\\Colors\\Error",
    "HKLM\\Software\\CatOS\\Terminal\\Colors\\Warning",
    "HKLM\\Software\\CatOS\\Terminal\\Colors\\Success",
    "HKLM\\Software\\CatOS\\Terminal\\Colors\\Info",
    "HKLM\\Software\\CatOS\\Terminal\\Colors\\Dim",
    "HKLM\\Software\\CatOS\\Terminal\\Colors\\Accent",
    "HKLM\\Software\\CatOS\\Terminal\\Colors\\Selection",
    "HKLM\\Software\\CatOS\\Terminal\\Colors\\StatusBg",
    "HKLM\\Software\\CatOS\\Terminal\\Colors\\StatusFg",
    "HKLM\\System\\Crash\\Colors\\Background",
    "HKLM\\System\\Crash\\Colors\\Foreground",
    "HKLM\\System\\Crash\\Colors\\Title",
    "HKLM\\System\\Crash\\Colors\\Hint",
    "HKLM\\System\\Crash\\Colors\\Code"
};

static const char *const k_names[PAL_COUNT] = {
    "background", "foreground", "prompt-user", "prompt-path", "prompt-symbol",
    "command", "argument", "error", "warning", "success", "info", "dim",
    "accent", "selection", "status-bg", "status-fg",
    "bsod-bg", "bsod-fg", "bsod-title", "bsod-hint", "bsod-code"
};

/* The v2.37 modern terminal theme, now only a fallback. */
static const uint32_t k_defaults[PAL_COUNT] = {
    /* v2.72: the canvas MUST match render.c's built-in theme canvas
     * (g_dark_tokens[0], #0A0E14).  Two different "backgrounds" was the
     * root cause of the half-painted-frame artifact. */
    0x0A0E14, 0xDCDCDC, 0x7EE787, 0x60A5FA, 0x7EE787,
    0xFFFFFF, 0xDCDCDC, 0xF85149, 0xE3B341, 0x56D364,
    0x39C5CF, 0x8B949E, 0xD299DD, 0x264F78, 0x161B22, 0x8B949E,
    0x0A1A3A, 0xDCE6F2, 0xFFFFFF, 0x8FB8FF, 0xFFD866
};

/* Statically initialised to the defaults: a panic before pal_init() must
 * still paint a readable crash screen (black-on-black is not acceptable). */
static uint32_t g_rgb[PAL_COUNT] = {
    0x0A0E14, 0xDCDCDC, 0x7EE787, 0x60A5FA, 0x7EE787,
    0xFFFFFF, 0xDCDCDC, 0xF85149, 0xE3B341, 0x56D364,
    0x39C5CF, 0x8B949E, 0xD299DD, 0x264F78, 0x161B22, 0x8B949E,
    0x0A1A3A, 0xDCE6F2, 0xFFFFFF, 0x8FB8FF, 0xFFD866
};
static char     g_hex[PAL_COUNT][8];
static char     g_scheme[8] = "dark";

static int strcmp_ci(const char *a, const char *b);

static int parse_hex6(const char *s, uint32_t *out) {
    if (!s) return -1;
    if (*s == '#') s++;
    uint32_t v = 0;
    for (int i = 0; i < 6; i++) {
        char c = s[i];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return -1;
        v = (v << 4) | (uint32_t)d;
    }
    if (s[6] != 0) return -1;          /* no trailing junk */
    *out = v;
    return 0;
}

static void rgb_to_hex(uint32_t rgb, char out[8]) {
    static const char hx[] = "0123456789ABCDEF";
    out[0] = '#';
    for (int i = 0; i < 6; i++) {
        uint8_t v = (uint8_t)((rgb >> (4 * (5 - i))) & 0xF);
        out[1 + i] = hx[v];
    }
    out[7] = 0;
}

/* Program the CLUT slot for a role.  Every backend reaches the slot
 * through render_clut(); text mode quantises at paint time. */
static void program_slot(int role) {
    con_set_palette((uint8_t)(PAL_SLOT_BASE + role), g_rgb[role]);
}

int pal_init(void) {
    int fallbacks = 0;
    for (int i = 0; i < PAL_COUNT; i++) {
        uint32_t v = 0;
        const char *s = hive_sz(k_console_paths[i], "");
        /* absent -> seed the default into the hive (one source of truth) */
        if (!s || !*s) {
            rgb_to_hex(k_defaults[i], g_hex[i]);
            hive_set_sz(k_console_paths[i], g_hex[i]);
            g_rgb[i] = k_defaults[i];
            fallbacks++;
        } else if (parse_hex6(s, &v) != 0) {
            /* present but malformed: do NOT trust it, use the default and
             * record the fallback so `theme` can report it */
            g_rgb[i] = k_defaults[i];
            rgb_to_hex(k_defaults[i], g_hex[i]);
            fallbacks++;
        } else {
            g_rgb[i] = v;
            rgb_to_hex(v, g_hex[i]);
        }
        program_slot(i);
    }
    /* keep the console's own style in sync with the roles */
    con_set_style_rgb(g_rgb[PAL_FG], g_rgb[PAL_BG], 0);
    return fallbacks;
}

int pal_reload(void) { return pal_init(); }

int pal_reset(void) {
    for (int i = 0; i < PAL_COUNT; i++) {
        g_rgb[i] = k_defaults[i];
        rgb_to_hex(g_rgb[i], g_hex[i]);
        hive_set_sz(k_console_paths[i], g_hex[i]);
        program_slot(i);
    }
    con_set_style_rgb(g_rgb[PAL_FG], g_rgb[PAL_BG], 0);
    return 0;
}

uint32_t pal_rgb(int role) {
    if (role < 0 || role >= PAL_COUNT) return 0xFFFFFF;
    return g_rgb[role];
}

uint8_t pal_vga(int role) {
    if (role < 0 || role >= PAL_COUNT) return VGA_WHITE;
    return (uint8_t)(PAL_SLOT_BASE + role);
}

const char *pal_hex(int role) {
    if (role < 0 || role >= PAL_COUNT) return "#FFFFFF";
    return g_hex[role];
}

const char *pal_path(int role) {
    if (role < 0 || role >= PAL_COUNT) return "";
    return k_console_paths[role];
}

const char *pal_name(int role) {
    if (role < 0 || role >= PAL_COUNT) return "?";
    return k_names[role];
}

int pal_set_rgb(int role, uint32_t rgb) {
    if (role < 0 || role >= PAL_COUNT) return -1;
    g_rgb[role] = rgb & 0xFFFFFFu;
    rgb_to_hex(g_rgb[role], g_hex[role]);
    program_slot(role);
    if (role == PAL_BG || role == PAL_FG)
        con_set_style_rgb(g_rgb[PAL_FG], g_rgb[PAL_BG], 0);
    return hive_set_sz(k_console_paths[role], g_hex[role]);
}

int pal_set(int role, const char *spec) {
    if (role < 0 || role >= PAL_COUNT || !spec || !*spec) return -1;
    uint32_t v = 0;
    if (parse_hex6(spec, &v) != 0 && con_parse_color(spec, &v) != 0) return -1;
    return pal_set_rgb(role, v);
}

/* ---- schemes ---------------------------------------------------------- */
static int apply_rgb(int role, uint32_t rgb) { return pal_set_rgb(role, rgb); }

int pal_apply_scheme(const char *name, int with_crash) {
    if (!name || !*name) return -1;

    if (!strcmp_ci(name, "mono")) {
        apply_rgb(PAL_BG, 0x000000);
        apply_rgb(PAL_FG, 0xCCCCCC);
        apply_rgb(PAL_PROMPT_USER, 0xFFFFFF);
        apply_rgb(PAL_PROMPT_PATH, 0xAAAAAA);
        apply_rgb(PAL_PROMPT_SYMBOL, 0xFFFFFF);
        apply_rgb(PAL_COMMAND, 0xFFFFFF);
        apply_rgb(PAL_ARGUMENT, 0xCCCCCC);
        apply_rgb(PAL_ERROR, 0xFFFFFF);
        apply_rgb(PAL_WARNING, 0xDDDDDD);
        apply_rgb(PAL_SUCCESS, 0xFFFFFF);
        apply_rgb(PAL_INFO, 0xBBBBBB);
        apply_rgb(PAL_DIM, 0x777777);
        apply_rgb(PAL_ACCENT, 0xFFFFFF);
    } else if (!strcmp_ci(name, "light")) {
        apply_rgb(PAL_BG, 0xF6F6F4);
        apply_rgb(PAL_FG, 0x24292F);
        apply_rgb(PAL_PROMPT_USER, 0x116329);
        apply_rgb(PAL_PROMPT_PATH, 0x0550AE);
        apply_rgb(PAL_PROMPT_SYMBOL, 0x116329);
        apply_rgb(PAL_COMMAND, 0x1F2328);
        apply_rgb(PAL_ARGUMENT, 0x3F4448);
        apply_rgb(PAL_ERROR, 0xCF222E);
        apply_rgb(PAL_WARNING, 0x9A6700);
        apply_rgb(PAL_SUCCESS, 0x1A7F37);
        apply_rgb(PAL_INFO, 0x0969DA);
        apply_rgb(PAL_DIM, 0x6E7781);
        apply_rgb(PAL_ACCENT, 0x8250DF);
        apply_rgb(PAL_SELECTION, 0xDDE7F3);
        apply_rgb(PAL_STATUS_BG, 0xE6E8EA);
        apply_rgb(PAL_STATUS_FG, 0x57606A);
    } else if (!strcmp_ci(name, "solarized")) {
        apply_rgb(PAL_BG, 0x002B36);
        apply_rgb(PAL_FG, 0x93A1A1);
        apply_rgb(PAL_PROMPT_USER, 0x859900);
        apply_rgb(PAL_PROMPT_PATH, 0x268BD2);
        apply_rgb(PAL_PROMPT_SYMBOL, 0x859900);
        apply_rgb(PAL_COMMAND, 0xFDF6E3);
        apply_rgb(PAL_ARGUMENT, 0x93A1A1);
        apply_rgb(PAL_ERROR, 0xDC322F);
        apply_rgb(PAL_WARNING, 0xB58900);
        apply_rgb(PAL_SUCCESS, 0x859900);
        apply_rgb(PAL_INFO, 0x2AA198);
        apply_rgb(PAL_DIM, 0x586E75);
        apply_rgb(PAL_ACCENT, 0x6C71C4);
        apply_rgb(PAL_SELECTION, 0x073642);
        apply_rgb(PAL_STATUS_BG, 0x073642);
        apply_rgb(PAL_STATUS_FG, 0x93A1A1);
    } else if (!strcmp_ci(name, "dark")) {
        for (int i = 0; i <= PAL_STATUS_FG; i++) apply_rgb(i, k_defaults[i]);
    } else {
        return -1;
    }

    if (with_crash) {
        if (!strcmp_ci(name, "light")) {
            apply_rgb(PAL_BSOD_BG, 0x7A0C12);
            apply_rgb(PAL_BSOD_FG, 0xFFE9E9);
        } else if (!strcmp_ci(name, "mono")) {
            apply_rgb(PAL_BSOD_BG, 0x101010);
            apply_rgb(PAL_BSOD_FG, 0xE8E8E8);
        } else {
            for (int i = PAL_BSOD_BG; i < PAL_COUNT; i++)
                apply_rgb(i, k_defaults[i]);
        }
    }
    /* remember the scheme name (an arbitrary custom palette reads as
     * "custom"; pal_scheme_name() reports whatever is stored) */
    int n = 0;
    while (name[n] && n < 7) { g_scheme[n] = name[n]; n++; }
    g_scheme[n] = 0;
    hive_set_sz("HKLM\\Software\\CatOS\\Terminal\\Theme", g_scheme);
    /* v2.67: a theme is a persisted setting, not a session flag.  Without
     * this the palette went back to the previous scheme on every reboot
     * while `hive chk` still reported the change as dirty. */
    hive_save();
    /* v2.72: repaint the whole frame with the NEW background.  Without this
     * only the cells that later got written carried the new colour and the
     * untouched area kept the old one -- the "font background is not the
     * background" and "leftover rectangle" complaints on VMware. */
    con_repaint_frame();
    return 0;
}

const char *pal_scheme_name(void) {
    const char *s = hive_sz("HKLM\\Software\\CatOS\\Terminal\\Theme", "custom");
    return (s && *s) ? s : "custom";
}

static int strcmp_ci(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
        if (ca != cb) return (int)(unsigned char)ca - (int)(unsigned char)cb;
        a++; b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}
