/*
 * dash.c -- v2.67 TUI dashboard.
 *
 * Not eye candy: this screen exercises every widget added to the toolkit
 * (tabs, section rules, gauges, key/value rows, sparkline, hint bar) on a
 * real back-end, and proves the cell-grid widgets cooperate with the dirty
 * tracker -- switching tabs only repaints the cells that change.
 */
#include <stdint.h>
#ifndef NULL
#define NULL ((void *)0)
#endif

#include "render.h"
#include "palette.h"
#include "tui.h"
#include "hw.h"
#include "hive.h"
#include "sec.h"
#include "user.h"
#include "task.h"
#include "keyboard.h"
#include "fs.h"
#include "vga.h"
#include "dash.h"

static void d_cat(char *d, int max, const char *s) {
    int n = 0; while (d[n]) n++;
    int i = 0;
    while (s && s[i] && n + i < max - 1) { d[n + i] = s[i]; i++; }
    d[n + i] = 0;
}
static void d_num(char *d, int max, int v) {
    char b[16]; int i = 0;
    if (v < 0) { d_cat(d, max, "-"); v = -v; }
    if (!v) b[i++] = '0';
    while (v > 0 && i < 15) { b[i++] = (char)('0' + v % 10); v /= 10; }
    while (i > 0) { char c[2]; c[0] = b[--i]; c[1] = 0; d_cat(d, max, c); }
}

#define DTABS 4

static const char *const TABNAMES[DTABS] = { "System", "Storage", "Network", "Security" };

static void draw_tab(int tab, int uid, uint32_t caps) {
    int cols = con_cols(), rows = con_rows();
    if (cols < 40 || rows < 16) {
        tui_label(0, 0, pal_rgb(PAL_WARNING), "dash: window too small (needs 40x16)");
        return;
    }
    int w = cols;
    uint32_t bg = pal_rgb(PAL_BG), fg = pal_rgb(PAL_FG);
    uint32_t acc = pal_rgb(PAL_ACCENT), dim = pal_rgb(PAL_DIM);
    char b[80], k[48];

    tui_hintbar(0, w, pal_rgb(PAL_STATUS_FG), pal_rgb(PAL_STATUS_BG),
                " CatOS dash   system | storage | network | security");
    {
        char ver[40]; ver[0] = 0;
        d_cat(ver, sizeof(ver), "v2.67 ");
        d_cat(ver, sizeof(ver), con_backend_name());
        int vn = 0; while (ver[vn]) vn++;
        if (vn < w - 4)
            tui_label_bg(w - vn - 1, 0, acc, pal_rgb(PAL_STATUS_BG), ver);
    }
    tui_tabs(1, 1, w, TABNAMES, DTABS, tab, acc, dim);
    tui_hline_sep(0, 2, w, dim);

    int y = 4;
    int pw = w - 2;

    if (tab == 0) {
        tui_section(1, y, pw, acc, "processor"); y += 2;
        b[0] = 0; d_num(b, sizeof(b), hw_num(HW_CPU_MHZ)); d_cat(b, sizeof(b), " MHz");
        tui_kv(1, y++, pw, dim, fg, "clock", b);
        if (hw_str(HW_CPU_BRAND_S, k, sizeof(k)) > 0) tui_kv(1, y++, pw, dim, fg, "brand", k);
        b[0] = 0; d_num(b, sizeof(b), hw_num(HW_CPU_CORES)); d_cat(b, sizeof(b), " logical core(s)");
        tui_kv(1, y++, pw, dim, fg, "smp", b); y++;

        tui_section(1, y, pw, acc, "memory"); y += 2;
        int used = hw_num(HW_MEM_USED_KB), freek = hw_num(HW_MEM_FREE_KB);
        int tot = used + freek;
        tui_gauge(1, y++, pw, tot > 0 ? (used * 100) / tot : 0, pal_rgb(PAL_WARNING), bg, "frames used");
        b[0] = 0; d_num(b, sizeof(b), used / 1024); d_cat(b, sizeof(b), " / ");
        d_num(b, sizeof(b), tot / 1024); d_cat(b, sizeof(b), " MB");
        tui_kv(1, y++, pw, dim, fg, "pmm", b); y++;

        tui_section(1, y, pw, acc, "kernel"); y += 2;
        b[0] = 0; d_num(b, sizeof(b), hw_num(HW_TASKS)); tui_kv(1, y++, pw, dim, fg, "live tasks", b);
        b[0] = 0; d_num(b, sizeof(b), hw_num(HW_UPTIME_MS) / 1000); d_cat(b, sizeof(b), " s");
        tui_kv(1, y++, pw, dim, fg, "uptime", b);
        b[0] = 0; d_num(b, sizeof(b), hw_num(HW_MEM_KERNEL_KB) / 1024); d_cat(b, sizeof(b), " MB image");
        tui_kv(1, y++, pw, dim, fg, "kernel", b);
    } else if (tab == 1) {
        tui_section(1, y, pw, acc, "volume"); y += 2;
        b[0] = 0; d_num(b, sizeof(b), hw_num(HW_DISK_MB)); d_cat(b, sizeof(b), " MB");
        tui_kv(1, y++, pw, dim, fg, "ide disk", b);
        tui_kv(1, y++, pw, dim, fg, "fat32", fs_fs_ready() ? "ready at /" : "not formatted"); y++;

        tui_section(1, y, pw, acc, "registry hive"); y += 2;
        hive_stat_t st;
        hive_stat(&st);
        b[0] = 0; d_num(b, sizeof(b), (int)st.nodes); d_cat(b, sizeof(b), " nodes");
        tui_kv(1, y++, pw, dim, fg, "tree", b);
        b[0] = 0; d_num(b, sizeof(b), (int)st.values); d_cat(b, sizeof(b), " values");
        tui_kv(1, y++, pw, dim, fg, "content", b);
        tui_kv(1, y++, pw, dim, fg, "image", st.loaded ? "loaded + crc ok" : "defaults (unsaved)");
        b[0] = 0; d_num(b, sizeof(b), (int)st.generation); d_cat(b, sizeof(b), " writes");
        tui_kv(1, y++, pw, dim, fg, "generation", b);
        tui_label(1, y, dim, "edit: hive set HKLM\\Software\\CatOS\\Terminal\\Colors\\Command \"#RRGGBB\"");
    } else if (tab == 2) {
        tui_section(1, y, pw, acc, "interface"); y += 2;
        char model[40];
        if (hw_str(HW_NIC_MODEL_S, model, sizeof(model)) > 0)
            tui_kv(1, y++, pw, dim, fg, "nic", model);
        tui_kv(1, y++, pw, dim, fg, "link", hw_num(HW_NIC_UP) ? "up" : "down");
        char ip[32];
        if (hw_str(HW_NIC_IP_S, ip, sizeof(ip)) > 0)
            tui_kv(1, y++, pw, dim, fg, "ip", ip);
        y++;

        tui_section(1, y, pw, acc, "activity"); y += 2;
        static uint8_t sp[40];
        uint32_t t = (uint32_t)hw_num(HW_TICKS);
        for (int i = 0; i < 40; i++) sp[i] = (uint8_t)((t / 5 + i * 17) % 60 + 10);
        tui_spark(1, y, pw, sp, 40, pal_rgb(PAL_INFO), bg); y++;
        tui_label(1, y, dim, "sparkline samples the PIT for now; net counters land in v2.68");
    } else {
        tui_section(1, y, pw, acc, "session"); y += 2;
        char caps_s[64];
        sec_caps_str(caps, caps_s, sizeof(caps_s));
        tui_kv(1, y++, pw, dim, fg, "caps", caps_s);
        const user_rec_t *u = user_by_uid(uid);
        tui_kv(1, y++, pw, dim, fg, "identity", u ? u->name : (uid == 0 ? "root" : "?"));
        y++;
        tui_section(1, y, pw, acc, "policy"); y += 2;
        tui_kv(1, y++, pw, dim, fg, "exec strict",
               hive_dw("HKSEC\\Policy\\Exec\\Strict", 1) ? "on (writable binaries refused)" : "off");
        tui_kv(1, y++, pw, dim, fg, "unknown binary",
               hive_dw("HKSEC\\Policy\\UnknownBinary\\AllowUnsigned", 1) ? "unsigned allowed" : "signature required");
        char need[48]; sec_caps_str(sec_required("shutdown", NULL), need, sizeof(need));
        tui_kv(1, y++, pw, dim, fg, "shutdown needs", need);
        char need2[48]; sec_caps_str(sec_required("format", NULL), need2, sizeof(need2));
        tui_kv(1, y++, pw, dim, fg, "format needs", need2);
        y++;
        tui_label(1, y++, dim, "full trail: `audit -n 20`   rebuild: `hive chk`");
        tui_label(1, y++, dim, "policy strings that do not parse fail closed (13)");
    }

    tui_hintbar(rows - 1, w, pal_rgb(PAL_STATUS_FG), pal_rgb(PAL_STATUS_BG),
                " q quit    tab next    1-4 jump    r refresh");
}

void dash_run(int uid, uint32_t caps) {
    int tab = 0, quit = 0;
    con_clear();
    draw_tab(tab, uid, caps);
    while (!quit) {
        int k = kb_getkey_timed(5);
        if (k < 0) continue;
        if (k == 'q' || k == 'Q' || k == 0x1B) quit = 1;
        else if (k == '\t') { tab = (tab + 1) % DTABS; draw_tab(tab, uid, caps); }
        else if (k >= '1' && k <= '0' + DTABS) { tab = k - '1'; draw_tab(tab, uid, caps); }
        else if (k == 'r' || k == 'R') draw_tab(tab, uid, caps);
    }
    con_clear();
    vga_write("dash: closed (reopen with `dash`)\n");
}
