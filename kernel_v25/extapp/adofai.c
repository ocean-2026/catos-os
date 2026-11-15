/* adofai.c - ADoFAI-mini text rhythm game for CatOS (CE01).
 * Imported from the 2026-11-11 handover zip (game.c) and updated for
 * kernel v2.14:
 *   - the ring0-era "mask IRQ1 + poll PS/2 ports" hack is GONE: the
 *     kernel now exports slot 64 pollkey() (non-blocking, fully decoded
 *     keys), so the game runs as an ordinary ring3 citizen and the
 *     keyboard keeps serving everyone else.
 *   - added beat sounds through the v2.11 SB16 slots (snd_tone); on a
 *     machine without an SB16 the game stays silent, not broken.
 * All-integer timing, file-level static buffers.
 */
typedef unsigned int   u32;
typedef unsigned short u16;
typedef unsigned char  u8;
typedef signed int     s32;

/* Real ABI via the kernel header (65 slots, v2.14): gives the game
 * pollkey() and the snd_* family instead of hand-maintained typedefs. */
#include "ce_abi.h"
catos_api_t *__ce_api __attribute__((section(".ce_api"))) = 0;
#define api __ce_api


#define MAX_TILES 256
#define LEADIN_MS 1200

/* color constants */
#define C_WHITE 7
#define C_GREEN 2
#define C_YELLOW 14
#define C_RED 4
#define C_BLUE 9
#define C_BRED 12

struct tile {
    int angle;      /* 0..345 (mult of 15), or 999 = midspin */
    int twirl;      /* flip sweep direction after this tile */
    int set_bpm;    /* 0=none, else absolute bpm from this tile onward */
    int set_speed;  /* 0=none, else multiplier pct (100=1x) */
};

/* ---- file-level static buffers (kernel stack is only 8KiB) ---- */
static char catbuf[8192];
static struct tile tiles[MAX_TILES];
static int  hit[MAX_TILES];    /* absolute hit time ms */
static int  wbpm[MAX_TILES];   /* effective bpm at tile */
static int  wP[MAX_TILES];     /* Perfect window ms (+-30deg) */
static int  wEP[MAX_TILES];    /* E/LPerfect window ms (+-45deg) */
static int  wE[MAX_TILES];    /* Early/Late window ms (+-60deg) */
static int  tdir[MAX_TILES];   /* sweep direction at tile (+1/-1) */
static int  nb;                /* tile count */
static char title[48];
static int  init_bpm;

/* play state */
static int  now, last_draw, cur;
static int  score, pcount, judged, combo, mp;
static int  won, failed;
static const char *jtext;
static int  jcol;
/* #60 additions */
static int  last_err_ms;        /* signed ms of the last non-miss hit      */
static int  last_err_set;       /* 0 = nothing judged yet                  */
static int  cd_phase = -1;      /* countdown shown last frame (3,2,1,0=GO) */
static int  prev_jtext_len = -1;
static int  prev_hud;           /* packed HUD fields, to skip repaints     */
static int  has_gotoxy;         /* api->gotoxy available (v2.21 kernel)    */

/* line builder */
static char rb[96];
static int  rl;


static void w(const char *s) { api->write(s); }
static void pc(char c) { api->putc(c); }
static void pads(int n) { int i; for (i = 0; i < n; i++) pc(' '); }

static const char *i2a(int v) {
    static char b[12];
    int i = 11; int neg = 0;
    b[11] = 0;
    if (v < 0) { neg = 1; v = -v; }
    if (v == 0) { b[--i] = '0'; }
    while (v > 0) { b[--i] = '0' + (v % 10); v /= 10; }
    if (neg) b[--i] = '-';
    return b + i;
}

static int smatch(const char *a, const char *b) {
    while (*b) { if (*a != *b) return 0; a++; b++; }
    return 1;
}
static char *findstr(const char *buf, const char *key) {
    const char *p = buf;
    while (*p) { if (smatch(p, key)) return (char *)p; p++; }
    return 0;
}

/* ---- schedule hit times + windows (all integer, remainder carry) ---- */
static void schedule(void) {
    int curbpm = init_bpm;
    int rem = 0;
    int dir = 1;
    int i;
    hit[0] = LEADIN_MS;
    for (i = 0; i < nb; i++) {
        int bpm = curbpm;
        if (bpm < 1) bpm = 1;
        tdir[i] = dir;
        wbpm[i] = bpm;
        {
            int p = 10000 / bpm;   if (p < 25)  p = 25;   wP[i] = p;
            int ep = 15000 / bpm;  if (ep < 30) ep = 30;  wEP[i] = ep;
            int e = 20000 / bpm;   if (e < 65)  e = 65;   wE[i] = e;
        }
        if (i + 1 < nb) {
            int beat = 60000 / bpm;
            rem += 60000 % bpm;
            if (rem >= bpm) { beat += rem / bpm; rem %= bpm; }
            hit[i + 1] = hit[i] + beat;
        }
        if (tiles[i].set_bpm) curbpm = tiles[i].set_bpm;
        if (tiles[i].set_speed) curbpm = curbpm * tiles[i].set_speed / 100;
        if (tiles[i].twirl) dir = -dir;
    }
}

/* ---- .tadofai parser (line based) ---- */
static int parse_tado(const char *buf) {
    const char *p = buf;
    int i;
    init_bpm = 120; nb = 0; title[0] = 0;
    while (*p) {
        char line[128]; int li = 0;
        while (*p && *p != '\n' && li < 127) line[li++] = *p++;
        line[li] = 0;
        if (*p == '\n') p++;
        {
            char *s = line;
            while (*s == ' ' || *s == '\t') s++;
            if (*s == 0 || *s == '#') continue;
            if (smatch(s, "title:")) {
                char *q = (char *)s + 6; int ti = 0;
                while (*q == ' ') q++;
                while (*q && *q != '\r' && ti < 46) title[ti++] = *q++;
                title[ti] = 0;
                continue;
            }
            if (smatch(s, "bpm:")) {
                int v = 0; char *q = (char *)s + 4;
                while (*q == ' ') q++;
                while (*q >= '0' && *q <= '9') v = v * 10 + (*q++ - '0');
                if (v > 0) init_bpm = v;
                continue;
            }
            if (smatch(s, "tile")) {
                char *q = (char *)s + 4;
                int ang = 0;
                while (*q == ' ') q++;
                while (*q >= '0' && *q <= '9') ang = ang * 10 + (*q++ - '0');
                if (ang != 999 && (ang > 345 || ang % 15 != 0)) return -1;
                if (nb >= MAX_TILES) return -1;
                tiles[nb].angle = ang;
                tiles[nb].twirl = 0; tiles[nb].set_bpm = 0; tiles[nb].set_speed = 0;
                while (*q == ' ') q++;
                while (*q) {
                    while (*q == ' ') q++;
                    if (smatch(q, "twirl")) { tiles[nb].twirl = 1; q += 5; }
                    else if (smatch(q, "speed")) {
                        int v = 0; q += 5;
                        while (*q == ' ') q++;
                        while (*q >= '0' && *q <= '9') v = v * 10 + (*q++ - '0');
                        tiles[nb].set_speed = v;
                    }
                    else if (smatch(q, "bpm")) {
                        int v = 0; q += 3;
                        while (*q == ' ') q++;
                        while (*q >= '0' && *q <= '9') v = v * 10 + (*q++ - '0');
                        tiles[nb].set_bpm = v;
                    }
                    else break;
                }
                nb++;
                continue;
            }
            /* unknown line: ignore */
        }
    }
    for (i = 0; i < nb; i++) (void)0;
    if (nb == 0) return -1;
    return 0;
}

/* ---- .adofai mini subset parser (byte scan, <=3KB) ---- */
static int read_int_skip(char **pp) {
    char *q = *pp;
    int sign = 1, v = 0, any = 0;
    while (*q && !(*q >= '0' && *q <= '9') && *q != '-') q++;
    if (*q == '-') { sign = -1; q++; }
    while (*q >= '0' && *q <= '9') { v = v * 10 + (*q - '0'); q++; any = 1; }
    *pp = q;
    return any ? sign * v : 0;
}

static int parse_adofai(const char *buf) {
    char *p;
    init_bpm = 120; nb = 0; title[0] = 0;
    p = findstr(buf, "angleData");
    if (!p) {
        if (findstr(buf, "pathData")) return -2;
        return -1;
    }
    p = findstr(p, "[");
    if (!p) return -1;
    p++;
    while (*p && nb < MAX_TILES) {
        while (*p && *p != ']' && !(*p >= '0' && *p <= '9') && *p != '-') p++;
        if (*p == ']' || !*p) break;
        {
            int neg = 0, v = 0;
            if (*p == '-') { neg = 1; p++; }
            while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
            if (neg) v = -v;
            tiles[nb].angle = v;
            tiles[nb].twirl = 0; tiles[nb].set_bpm = 0; tiles[nb].set_speed = 0;
            nb++;
        }
    }
    {
        char *b = findstr(buf, "beatsPerMinute");
        if (b) { char *q = b; while (*q && *q != ':') q++; if (*q == ':') q++; init_bpm = read_int_skip(&q); }
    }
    {
        char *e = (char *)buf;
        while ((e = findstr(e, "\"eventType\"")) != 0) {
            char *set = findstr(e, "SetSpeed");
            char *tw = findstr(e, "Twirl");
            int isSet = (set && (set - e) < 300);
            int isTw = (tw && (tw - e) < 300 && !isSet);
            char *fl = findstr(e, "\"floor\"");
            int floor = -1;
            if (fl) { char *q = fl; while (*q && *q != ':') q++; if (*q == ':') q++; floor = read_int_skip(&q); }
            if (isSet && floor >= 0 && floor < MAX_TILES) {
                char *mul = findstr(e, "Multiplier");
                if (mul && (mul - e) < 300) {
                    char *bb = findstr(e, "bpmMultiplier");
                    if (bb) { char *q = bb; while (*q && *q != ':') q++; if (*q == ':') q++; tiles[floor].set_speed = read_int_skip(&q); }
                } else {
                    char *bb = findstr(e, "beatsPerMinute");
                    if (bb) { char *q = bb; while (*q && *q != ':') q++; if (*q == ':') q++; tiles[floor].set_bpm = read_int_skip(&q); }
                }
            } else if (isTw && floor >= 0 && floor < MAX_TILES) {
                tiles[floor].twirl = 1;
            }
            e++;
        }
    }
    if (nb == 0) return -1;
    return 0;
}

/* ---- frame rendering ---- */
static void rb_reset(void) { rl = 0; rb[0] = 0; }
static void rb_s(const char *s) { while (*s) { rb[rl] = *s++; rb[rl + 1] = 0; rl++; } }
static void rb_i(int v) { rb_s(i2a(v)); }
static void rb_pad(void) { while (rl < 80) { rb[rl] = ' '; rb[rl + 1] = 0; rl++; } }

/* #60 (a): dirty-region redraw.  Clearing the screen every 33 ms and then
 * repainting 25 lines shows the blank frame in between -- that IS the
 * flicker.  With the v2.21 gotoxy slot we repaint only the lines whose
 * content actually changed; on an older kernel we fall back to the old
 * full-frame path (correct, just less pretty).
 *
 * Static layout (drawn once per level):  row 0 title, rows 2-7 blank,
 * row 8 accuracy bar, row 11 "last error", row 12 hint, 13..24 blank.
 * Dynamic rows: 1 (HUD), 9 (sweep marker), 10 (judgement word). */

static void draw_title(void) {
    api->set_fg(C_WHITE);
    rb_reset(); rb_s(title[0] ? title : "Cat Beats"); rb_pad(); w(rb); w("\n");
}

static void draw_static(void) {
    int col;
    api->cls();
    draw_title();
    /* rows 1..7: leave row 1 for the HUD, 2..7 blank */
    w("\n\n\n\n\n\n\n");
    /* row 8: accuracy bar, columns 10..69 */
    pads(10);
    api->set_fg(C_RED);    for (col = 0; col < 15; col++) pc('=');
    api->set_fg(C_YELLOW); for (col = 0; col < 5;  col++) pc('=');
    api->set_fg(C_GREEN);  for (col = 0; col < 20; col++) pc('=');
    api->set_fg(C_YELLOW); for (col = 0; col < 5;  col++) pc('=');
    api->set_fg(C_RED);    for (col = 0; col < 15; col++) pc('=');
    api->set_fg(C_WHITE); pads(10); w("\n");
    /* row 9 marker (dynamic), row 10 judgement (dynamic), row 11 last err */
    w("\n\n\n");
    /* row 12: hint */
    rb_reset(); rb_s("  ANY KEY = TAP     ESC = QUIT  "); rb_pad(); w(rb); w("\n");
    w("\n\n\n\n\n\n\n\n\n\n\n");
}

static void draw_hud(int force) {
    int pct = (nb > 0) ? cur * 100 / nb : 0;
    int acc = (judged > 0) ? pcount * 100 / judged : 0;
    int pack = (cur << 16) ^ score ^ (acc << 8) ^ (combo << 4);
    if (!force && pack == prev_hud) return;
    prev_hud = pack;
    if (has_gotoxy) api->gotoxy(0, 1);
    api->set_fg(C_WHITE);
    rb_reset();
    rb_s("PROGRESS "); rb_i(pct); rb_s("%  SCORE "); rb_i(score);
    rb_s("  ACC "); rb_i(acc); rb_s("%  COMBO "); rb_i(combo);
    rb_pad(); w(rb); w("\n");
}

static void draw_marker(void) {
    int beat = 60000 / (wbpm[cur] > 0 ? wbpm[cur] : 100);
    int off = (now - hit[cur]) * 60 / beat;
    int col = 40 + tdir[cur] * off;
    if (col < 10) col = 10;
    if (col > 69) col = 69;
    if (has_gotoxy) api->gotoxy(0, 9);
    else     { int i; for (i = 0; i < 9; i++) w("\n"); }
    api->set_fg(C_WHITE);
    pads(col); pc('O'); pads(79 - col); w("\n");
}

static void draw_judge(void) {
    int jl = 0, lead;
    while (jtext[jl]) jl++;
    lead = 40 - jl / 2; if (lead < 0) lead = 0;
    if (has_gotoxy) api->gotoxy(0, 10);
    api->set_fg(jcol);
    pads(lead); w(jtext); pads(79 - lead - jl); w("\n");
}

/* row 11: signed timing of the last hit -- "12ms EARLY" means the player
 * pressed 12 ms before the tile beat.  This is what makes a rhythm game
 * learnable: the player can see which way they are drifting. */
static void draw_lasterr(void) {
    int v = last_err_ms;
    if (has_gotoxy) api->gotoxy(0, 11);
    else     { int i; for (i = 0; i < 2; i++) w("\n"); }
    api->set_fg(C_WHITE);
    if (!last_err_set) { pads(80); w("\n"); return; }
    rb_reset();
    rb_s("LAST ");
    if (v < 0) rb_s("-"); else rb_s("+");
    rb_i(v < 0 ? -v : v); rb_s(" ms  ");
    rb_s(v < 0 ? "EARLY (pressed too soon)" : "LATE  (pressed too late)");
    rb_pad(); w(rb); w("\n");
}

static void draw_countdown(int n) {
    /* big centred banner on rows 3..6, 3-2-1-GO */
    int row;
    for (row = 3; row <= 6; row++) {
        if (has_gotoxy) api->gotoxy(0, row);
        else { int i; for (i = 0; i < row; i++) w("\n"); }
        pads(80); w("\n");
    }
    if (has_gotoxy) api->gotoxy(0, 4);
    else { int i; for (i = 0; i < 4; i++) w("\n"); }
    api->set_fg(n > 0 ? C_YELLOW : C_GREEN);
    if (n > 0) {
        pads(39); pc('0' + n); pc(' '); pads(39);
    } else {
        pads(38); w("GO!"); pads(39);
    }
    w("\n");
}

static void draw(void) {
    if (has_gotoxy) {
        draw_hud(0);
        draw_marker();
        if (jtext[0]) draw_judge();
        if (last_err_set) draw_lasterr();
        return;
    }
    /* legacy full-frame path (kernel has no gotoxy): cls + repaint all */
    {
        int pct = (nb > 0) ? cur * 100 / nb : 0;
        int acc = (judged > 0) ? pcount * 100 / judged : 0;
        int beat = 60000 / (wbpm[cur] > 0 ? wbpm[cur] : 100);
        int off = (now - hit[cur]) * 60 / beat;
        int col = 40 + tdir[cur] * off, col2;
        int jl = 0, lead;
        api->cls();
        draw_title();
        api->set_fg(C_WHITE);
        rb_reset();
        rb_s("PROGRESS "); rb_i(pct); rb_s("%  SCORE "); rb_i(score);
        rb_s("  ACC "); rb_i(acc); rb_s("%  COMBO "); rb_i(combo);
        rb_pad(); w(rb); w("\n");
        w("\n\n\n\n\n\n");
        pads(10);
        api->set_fg(C_RED);    for (col2 = 0; col2 < 15; col2++) pc('=');
        api->set_fg(C_YELLOW); for (col2 = 0; col2 < 5;  col2++) pc('=');
        api->set_fg(C_GREEN);  for (col2 = 0; col2 < 20; col2++) pc('=');
        api->set_fg(C_YELLOW); for (col2 = 0; col2 < 5;  col2++) pc('=');
        api->set_fg(C_RED);    for (col2 = 0; col2 < 15; col2++) pc('=');
        api->set_fg(C_WHITE); pads(10); w("\n");
        if (col < 10) col = 10;
        if (col > 69) col = 69;
        pads(col); pc('O'); pads(79 - col); w("\n");
        api->set_fg(jcol);
        while (jtext[jl]) jl++;
        lead = 40 - jl / 2; if (lead < 0) lead = 0;
        pads(lead); w(jtext); pads(79 - lead - jl); w("\n");
        w("\n");
        rb_reset(); rb_s("  ANY KEY = TAP     ESC = QUIT  "); rb_pad(); w(rb); w("\n");
        w("\n\n\n\n\n\n\n\n\n\n\n");
    }
}

/* ---- input: kernel pollkey() (v2.14 slot 64) ----
 * The old build masked IRQ1 in the PIC and polled ports 0x64/0x60
 * directly (a ring0 trick the old CE ABI forced on us).  pollkey()
 * hands us fully decoded keys, never blocks, and the kernel keeps its
 * keyboard for itself -- no PIC surgery, no side effects. */

static void drain_kb(void) {
    while (api->pollkey() >= 0) ;
}

static int snd_ok(void) {
    return api->snd_info != 0 && api->snd_info(SND_INFO_READY) == 1;
}

/* beat sounds via the v2.11 SB16 slots; silent (not broken) without card */
static void beep(int hz, int ms) {
    if (snd_ok()) api->snd_tone(hz, ms);
}

/* returns the next key (ASCII or KB_* >0xFF); polls once per ms */
static int wait_key(void) {
    for (;;) {
        int k = api->pollkey();
        if (k >= 0) return k;
        api->delay(1);
    }
}

/* ---- result screen; returns 1 if ESC pressed (exit CE), 0 otherwise ---- */
static int show_result(void) {
    int pct = (nb > 0) ? cur * 100 / nb : 0;
    int pure = (won && pcount == judged && mp == 0);
    api->cls();
    w("\n\n\n");
    if (won) {
        api->set_fg(C_YELLOW);
        w("              Congratulations!\n\n");
        if (pure) { api->set_fg(C_GREEN); w("               Pure Perfect!\n\n"); }
    } else {
        api->set_fg(C_RED);
        w("                Game over!\n\n");
        w("               PROGRESS "); w(i2a(pct)); w("%\n\n");
    }
    api->set_fg(C_WHITE);
    w("          Restart!  press any key\n");
    w("              (ESC = exit game)\n");
    {
        int c = wait_key();
        return (c == 27) ? 1 : 0;     /* ESC = exit */
    }
}

/* ---- play one loaded level; returns 1 if ESC at result -> exit CE ---- */
static int play_level(void) {
    schedule();
    now = 0; last_draw = -100; cur = 0;
    score = 0; pcount = 0; judged = 0; combo = 0; mp = 0;
    won = 0; failed = 0;
    jtext = ""; jcol = C_WHITE;
    last_err_ms = 0; last_err_set = 0;
    prev_jtext_len = -1; prev_hud = -1; cd_phase = -1;
    has_gotoxy = (api->gotoxy != 0);

    if (has_gotoxy) draw_static();
    else            api->cls();

    /* #60 (b): 3-2-1-GO! countdown.  now stays 0 (hit[] is relative to the
     * first tile) -- the clock simply does not advance until "GO!", so a
     * stray key during the countdown is drained instead of judged. */
    {
        int cd, t;
        for (cd = 3; cd >= 1; cd--) {
            draw_countdown(cd);
            cd_phase = cd;
            for (t = 0; t < 1000; t++) {
                api->delay(1);
                if (api->pollkey() == 27) return 1;
            }
        }
        draw_countdown(0);                    /* GO! */
        cd_phase = 0;
        for (t = 0; t < 500; t++) {
            api->delay(1);
            if (api->pollkey() == 27) return 1;
        }
        drain_kb();
        /* wipe the banner rows 3..6 */
        if (has_gotoxy) {
            int row;
            for (row = 3; row <= 6; row++) {
                api->gotoxy(0, row);
                pads(80); w("\n");
            }
        } else {
            api->cls();
        }
        cd_phase = -1;
        draw();
    }

    while (1) {
        api->delay(1); now++;
        /* poll keys via the kernel; every key = a tap; ESC quits */
        {
            int presses = 0, esc = 0, k;
            while ((k = api->pollkey()) >= 0) {
                presses++;
                if (k == 27) esc = 1;
            }
            if (esc) return 1;   /* mid-game ESC -> exit */
            /* auto-pass midspin (999): occupies a beat, no key */
            if (cur < nb && tiles[cur].angle == 999 && now >= hit[cur]) { cur++; }
            if (presses > 0 && cur < nb) {
                int h = hit[cur], e = wE[cur], ep = wEP[cur], p = wP[cur];
                int dt = now - h;
                int adt = dt < 0 ? -dt : dt;
                if (dt < -e) {
                    jtext = "Multipress!!"; jcol = C_WHITE; mp++;
                    beep(180, 120);
                } else if (adt <= p) {
                    jtext = "Perfect!"; jcol = C_WHITE;
                    last_err_ms = dt; last_err_set = 1;
                    score += 2; pcount++; judged++; combo++; cur++;
                    beep(880, 50);
                } else if (adt <= ep) {
                    jtext = (dt < 0) ? "EPerfect!" : "LPerfect!";
                    jcol = (dt < 0) ? C_BLUE : C_BRED;
                    last_err_ms = dt; last_err_set = 1;
                    score += 1; judged++; combo++; cur++;
                    beep(740, 50);
                } else if (adt <= e) {
                    jtext = (dt < 0) ? "Early!" : "Late!";
                    jcol = C_YELLOW;
                    last_err_ms = dt; last_err_set = 1;
                    score += 1; judged++; combo++; cur++;
                    beep(520, 50);
                } else {
                    jtext = (dt < 0) ? "Early!!" : "Late!!";
                    jcol = C_RED; failed = 1;
                    beep(180, 150);
                }
            }
        }
        if (failed) break;
        if (cur >= nb) { won = 1; break; }
        /* miss: window closed without a press */
        if (now - hit[cur] > wE[cur]) { jtext = "Miss..."; jcol = C_RED; failed = 1; break; }
        if (now - last_draw >= 33) { draw(); last_draw = now; }
    }
    /* pollkey(): the result screen reads keys the same way */
    return show_result();
}

static int load_and_play(const char *path) {
    int n;
    api->cls();
    w("Loading "); w(path); w(" ...\n");
    n = api->cat(path, catbuf, 8191);
    if (n <= 0) {
        api->set_fg(C_RED);
        w("FILE NOT FOUND: "); w(path); w("\n\n");
        api->set_fg(C_WHITE); w("press any key...\n");
        wait_key();
        return 0;
    }
    {
        int r;
        if (findstr(catbuf, "angleData")) r = parse_adofai(catbuf);
        else r = parse_tado(catbuf);
        if (r == -2) {
            api->set_fg(C_RED); w("UNSUPPORTED: pathData\n\n");
            api->set_fg(C_WHITE); w("press any key...\n"); wait_key(); return 0;
        }
        if (r != 0) {
            api->set_fg(C_RED); w("LEVEL PARSE ERROR\n\n");
            api->set_fg(C_WHITE); w("press any key...\n"); wait_key(); return 0;
        }
    }
    return play_level();
}

__attribute__((section(".text.ce_entry")))
int ce_main(void) {
    int c;
    for (;;) {
        api->cls();
        w("===== ADOFAI (TEXT) =====\n\n");
        w("  [1] EASY Breeze      (72 tiles, bpm 90)\n");
        w("  [2] NORMAL Flow      (132 tiles, bpm 120)\n");
        w("  [3] HARD Surge       (204 tiles, bpm 160)\n");
        w("  [4] MARATHON Run     (200 tiles, bpm 180)\n");
        w("  [5] LOAD CUSTOM LEVEL\n\n");
        w("  ESC = EXIT\n\n");
        c = wait_key();                    /* ASCII: 27 ESC, '1'..'5' */
        if (c == 27) break;                /* ESC -> exit */
        if (c == '1') { beep(440, 60); if (load_and_play("/BIN/EASY.TAD")) break; }
        else if (c == '2') { beep(494, 60); if (load_and_play("/BIN/NORMAL.TAD")) break; }
        else if (c == '3') { beep(523, 60); if (load_and_play("/BIN/HARD.TAD")) break; }
        else if (c == '4') { beep(587, 60); if (load_and_play("/BIN/MARATHON.TAD")) break; }
        else if (c == '5') {
            char path[64];
            api->cls();
            w("Enter level path (.TAD / .AD):\n> ");
            path[0] = 0;
            api->readline(path, 63);
            if (path[0]) { if (load_and_play(path)) break; }
        }
    }
    drain_kb();
    return 0;   /* plain return: back to the CatOS shell */
}
