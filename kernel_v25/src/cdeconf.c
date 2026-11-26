/*
 * cdeconf.c -- v2.82 A1.4: /etc/cde.conf appearance configuration mirror.
 *
 * See include/cdeconf.h for the file format and precedence rules.  This
 * module deliberately adds no CE ABI entry and no shell command; it is the
 * file-backed mirror next to the hive registry, on the same line as
 * /etc/lang, /etc/env and /etc/style.
 */
#include "cdeconf.h"
#include "fs.h"
#include "hive.h"
#include "palette.h"
#include "kstring.h"
#include <stdint.h>

#define CDE_PATH "/etc/cde.conf"

static char g_scheme[16];   /* last theme= label read from the file ("" if none) */
static char g_accent[10];   /* last accent= value read ("#RRGGBB", "" if none)   */

/* ---- local freestanding helpers (mirror the shell/palette local ones) ----- */

static int cde_len(const char *s) {
    int n = 0;
    while (s && s[n]) n++;
    return n;
}

static int cde_eq_ci(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

static void cde_trim(char *s) {
    char *p = s;
    while (*p == ' ' || *p == '\t' || *p == '\r') p++;
    if (p != s) { char *d = s; while ((*d++ = *p++)); }
    int n = cde_len(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r'))
        s[--n] = 0;
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Accept "#RRGGBB" only (the mirror writes hex via pal_hex()). */
static int parse_hex6(const char *s, uint32_t *v) {
    if (!s || !v) return -1;
    if (*s != '#') return -1;
    s++;
    uint32_t r = 0;
    for (int i = 0; i < 6; i++) {
        int d = hex_digit(s[i]);
        if (d < 0) return -1;
        r = r * 16 + (uint32_t)d;
    }
    if (s[6] != 0) return -1;
    *v = r;
    return 0;
}

static int scheme_valid(const char *n) {
    return cde_eq_ci(n, "dark") || cde_eq_ci(n, "light") ||
           cde_eq_ci(n, "solarized") || cde_eq_ci(n, "mono");
}

const char *cdeconf_scheme(void) { return g_scheme; }

void cdeconf_load(void) {
    g_scheme[0] = 0;
    g_accent[0] = 0;
    if (!fs_fs_ready()) return;

    char buf[256];
    int n = fs_read_text(CDE_PATH, buf, (int)sizeof(buf) - 1);
    if (n <= 0) return;                 /* missing file -> compiled defaults */
    buf[n] = 0;

    /* The hive stays the primary source (v2.72 rule).  Adopt the file only
     * when the hive carries no scheme label yet; a label that reads
     * "custom" means the palette was hand-edited, also treat as unlabeled. */
    const char *hive_theme = hive_sz("HKLM\\Software\\CatOS\\Terminal\\Theme", "");
    int hive_labeled = hive_theme && *hive_theme &&
                       !cde_eq_ci(hive_theme, "custom");

    char *q = buf;
    while (*q) {
        char *line = q;
        while (*q && *q != '\n') q++;
        if (*q) *q++ = 0;
        cde_trim(line);
        if (!*line || *line == '#') continue;

        char *eq = line;
        while (*eq && *eq != '=') eq++;
        if (!*eq) continue;
        *eq = 0;
        char *key = line, *val = eq + 1;
        cde_trim(key);
        cde_trim(val);

        if (cde_eq_ci(key, "theme")) {
            int m = 0;
            while (val[m] && m < 15) { g_scheme[m] = val[m]; m++; }
            g_scheme[m] = 0;
            if (!hive_labeled && scheme_valid(val)) pal_apply_scheme(val, 0);
        } else if (cde_eq_ci(key, "accent")) {
            int m = 0;
            while (val[m] && m < 9) { g_accent[m] = val[m]; m++; }
            g_accent[m] = 0;
            uint32_t rgb;
            if (!hive_labeled && parse_hex6(val, &rgb) == 0)
                pal_set_rgb(PAL_ACCENT, rgb);
        }
        /* fontsize / taskbar: reserved keys.  The block desktop has no runtime
         * font-size state and the taskbar position is fixed to the bottom
         * edge; both are parsed (unknown keys do not error) but NOT applied.
         * Do not invent a fake read/write for them. */
    }
}

void cdeconf_save_theme(const char *scheme) {
    if (!fs_fs_ready() || !scheme || !*scheme) return;

    /* Preserve an existing accent= mirror line; otherwise mirror the live
     * PAL_ACCENT role. */
    char accent[10] = {0};
    char old[256];
    int on = fs_read_text(CDE_PATH, old, (int)sizeof(old) - 1);
    if (on > 0) {
        old[on] = 0;
        char *q = old;
        while (*q) {
            char *line = q;
            while (*q && *q != '\n') q++;
            if (*q) *q++ = 0;
            char *eq = line;
            while (*eq && *eq != '=') eq++;
            if (*eq) {
                *eq = 0;
                cde_trim(line); cde_trim(eq + 1);
                if (cde_eq_ci(line, "accent") && *(eq + 1)) {
                    int m = 0;
                    while (eq[1 + m] && m < 8) { accent[m] = eq[1 + m]; m++; }
                    accent[m] = 0;
                }
            }
        }
    }
    if (!accent[0]) {
        const char *h = pal_hex(PAL_ACCENT);
        int m = 0;
        while (h[m] && m < 8) { accent[m] = h[m]; m++; }
        accent[m] = 0;
    }

    char buf[256];
    int o = 0;
    const char *head = "# CatOS appearance mirror (v2.82 A1.4); hive stays primary.\n";
    for (int i = 0; head[i]; i++) buf[o++] = head[i];
    const char *tk = "theme=";
    for (int i = 0; tk[i]; i++) buf[o++] = tk[i];
    for (int i = 0; scheme[i] && o < (int)sizeof(buf) - 2; i++) buf[o++] = scheme[i];
    buf[o++] = '\n';
    const char *ak = "accent=";
    for (int i = 0; ak[i]; i++) buf[o++] = ak[i];
    for (int i = 0; accent[i] && o < (int)sizeof(buf) - 2; i++) buf[o++] = accent[i];
    buf[o++] = '\n';

    fs_mkdir("/etc");   /* volume missing the directory: backstop, like settings.c */
    fs_write_text(CDE_PATH, buf, (uint32_t)o);
}
