#include "shell.h"
#include "vga.h"
#include "render.h"      /* display front-end: mode policy + text styles */
#include "keyboard.h"
#include "ime.h"
#include "usb.h"
#include "rtc.h"
#include "io.h"
#include "fs.h"
#include "ata.h"
#include "fat.h"
#include "diag.h"       /* faultinfo: last-fault record */
#include "ce.h"
#include "net.h"
#include "task.h"
#include "hw.h"
#include "snd.h"        /* v2.11 SB16 info / self-test commands */
#include "gfx24.h"      /* v2.12 24bpp truecolour drawing surface */
#include "svga.h"       /* v2.20 VMware SVGA II framebuffer driver */
#include "e1000.h"       /* v2.21 #55 e1000 NIC state for netdiag */
#include "es1371.h"      /* v2.21 #56 ES1370/1371 audio state for sndinfo */
#include "lang.h"       /* v2.21 T50: en/zh user-visible messages      */

void spawn_and_maybe_wait(const char *path, int argc, char **argv, int bg, int uid);

/* ---------- tiny libc ---------- */
static int strcmp_(const char *a, const char *b) {
    while (*a && (*a == *b)) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static int slen_(const char *s) { int n = 0; while (s && s[n]) n++; return n; }

/* forward decls: the shared command dispatcher and the boot autostart
 * runner (/etc/autostart scripts, v2.17) */
static void shell_exec(char *cmd, char *arg);
static void boot_autostart(void);

static void itoa(int v, char *out, int width) {
    char tmp[12]; int i = 0, neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    if (v == 0) tmp[i++] = '0';
    while (v > 0) { tmp[i++] = '0' + (v % 10); v /= 10; }
    while (i < width) tmp[i++] = '0';
    if (neg) tmp[i++] = '-';
    int j = 0; while (i > 0) out[j++] = tmp[--i];
    out[j] = 0;
}

static void print_int(int v) {
    char b[12]; itoa(v, b, 0); vga_write(b);
}
static void print_hex(uint32_t v) {
    char b[10]; int i = 0;
    if (v == 0) { vga_write("0"); return; }
    while (v > 0) { b[i++] = "0123456789ABCDEF"[v & 0xF]; v >>= 4; }
    while (i > 0) { char c = b[--i]; vga_putc(c); }
}
static void print_pad(int v, int w) {
    char b[12]; itoa(v, b, w); vga_write(b);
}

/* ---------- multiboot info ---------- */
extern uint32_t multiboot_info_ptr;

typedef struct {
    uint32_t flags;
    uint32_t mem_lower, mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count, mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length, mmap_addr;
    uint32_t drives_length, drives_addr;
    uint32_t config_table;
    uint32_t boot_loader_name;
    uint32_t apm_table;
} multiboot_info_t;

static multiboot_info_t *mbi;

/* ---------- commands ---------- */
static void cmd_help(void) {
    vga_write(
        "CatOS v0.3 (five-pillar kernel) - commands:\n"
        "  help         show this message\n"
        "  clear        clear the screen\n"
        "  lang [en|zh] show/set UI language (default en, /etc/lang)\n"
        "  Up/Down history | Left/Right/Home/End edit the line\n"
        "  Ctrl+Up/Down   scroll back console output (any key returns)\n"
        "  echo TEXT    print text back\n"
        "  about        system info banner\n"
        "  autostart    re-run /etc/autostart scripts\n"
        "  uname        print OS name\n"
        "  date         show RTC date/time\n"
        "  mem          show RAM size (multiboot)\n"
        "  color NUM    set foreground color (0..255; text mode maps to 16)\n"
        "  -- display (front-end / back-ends) --\n"
        "  gfx          graphics mode (256-color styled text)\n"
        "  vbe          800x600x32 truecolour text (Bochs VBE / VMware SVGA)\n"
        "  gfx24|vbe24 800x600 24-bit-colour drawing surface + pixel demo\n"
        "  svgainfo     VMware SVGA II PCI/FIFO register dump\n"
        "  text         switch back - needs a physical Y confirmation\n"
        "  style SPEC   bold+italic+underline+inverse+strike+dim / off\n"
        "  rgb FG [BG] literal 0xRRGGBB colors (VBE: 16.7M colors)\n"
        "  palette I C  redefine CLUT[I] as RRGGBB\n"
        "  display      backend, grid, depth, CJK, glyph count\n"
        "  vdiag        raw VGA/VBE register diagnostic\n"
        "  -- Chinese (GB2312 display + pinyin input) --\n"
        "  zhdemo       built-in UTF-8 Chinese + truecolour demo\n"
        "  F2           toggle pinyin IME (works in shell and edit)\n"
        "  color NUM    set foreground palette index (0..255)\n"
        "  reboot       warm reboot the machine\n"
        "  hang         halt the CPU (QEMU: close window)\n"
        "  -- processes (ring3 tasks, preemptive) --\n"
        "  run FILE.ce [args..] [&]  execute a CE app (in background with &)\n"
        "  ps           list tasks (tid, uid, ring, state)\n"
        "  kill TID     terminate a task\n"
        "  sudo CMD     run a program as root (uid 0)\n"
        "  -- users & permissions (v2.13, initial user: root) --\n"
        "  whoami       current session user\n"
        "  id [USER]    uid/gid/capabilities of a user\n"
        "  lsuser       list accounts (/etc/passwd)\n"
        "  caps         capability bit legend + session caps\n"
        "  login U [PW] switch session user ('su' = alias)\n"
        "  useradd N [UID [GID [CAPS]]]  add user (CAP_USERADM)\n"
        "  userdel N | passwd N PW | chcaps HEX N\n"
        "  chmod OCT PATH | chown UID[:GID] PATH  (persistent /etc/acl)\n"
        "  uptime       seconds since boot + task count\n"
        "  top          live task monitor (CPU% per task, any key quits)\n"
        "  jobs         list background jobs ('run X &') with pending log\n"
        "  log TID      show a background job's captured output\n"
        "  log -f TID   follow that job until it exits\n"
        "  -- environment (v2.23, persisted in /etc/env) --\n"
        "  export K=V   set variable (empty V deletes); env lists all\n"
        "  unset K      delete variable; $K / ${K} expand in typed lines\n"
        "  PATH         drives bare-NAME lookup (default /bin)\n"
        "  hwinfo       full hardware dump (CPU/mem/disk/NIC/PCI)\n"
        "  -- audio (Sound Blaster 16, ISA) --\n"
        "  sndinfo      SB16 probe, DSP version, IRQ/DMA diagnostics\n"
        "  sndtest [HZ MS]  play a tone (default 440 Hz / 800 ms)\n"
        "  -- network (PCnet) --\n"
        "  curl URL     HTTP GET (http://IP[:port]/path)\n"
        "  ping IP      ICMP echo (3 tries)\n"
        "  dns NAME     resolve a hostname\n"
        "  udp IP PORT TEXT  send one UDP datagram and wait for a reply\n"
        "  netdiag      dump NIC/driver state\n"
        "  Bare NAME is looked up in cwd then /bin (PATH); 'run FILE' uses\n"
        "  the path verbatim with NO lookup (a bare NAME there is cwd-only).\n"
        "  -- filesystem (FAT32 on IDE, persistent) --\n"
        "  dir [PATH]   list a directory\n"
        "  cd [PATH]    change directory (use .. or /)\n"
        "  mkdir PATH   create a directory\n"
        "  touch FILE   create an empty file\n"
        "  cat FILE     print a file (text, up to 64 KiB)\n"
        "  rm FILE      delete a file\n"
        "  cp SRC DST   copy a file\n"
        "  mv SRC DST   move a file (copy + delete)\n"
        "  format       erase the FAT32 volume (y/n confirm)\n"
        "  fsck         verify/repair FAT #2 against FAT #1 (crash check)\n"
        "  faultinfo    show the last fatal fault (vector, CR2, PF bits)\n"
        "  edit FILE    nano-style full-screen editor\n"
        "               (arrows move, ^O save, ^X exit, ^K del line, ^U clear)\n"
    );
}

/* ---------- SB16 audio (v2.11) -----------------------------------------
 * The shell runs in ring0 with interrupts enabled, so it can call the
 * driver directly; ring3 programs go through the four v2.11 ABI slots.
 * To get sound out of QEMU >= 7.1 (-soundhw was removed):
 *   -audiodev wav,id=snd0,path=snd.wav
 *   -device sb16,iobase=0x220,irq=5,dma=1,dma16=5,audiodev=snd0
 * ---------------------------------------------------------------------- */
/* ---------- T50: language setting --------------------------------------- */
static void cmd_lang(const char *arg) {
    while (*arg == ' ') arg++;
    if (!*arg) {
        vga_write("lang: "); vga_write(lang_name());
        vga_write("   (usage: lang [en|zh])\n");
        return;
    }
    int l;
    if (arg[0] == 'e' && arg[1] == 'n' && (arg[2] == 0 || arg[2] == ' ')) l = LANG_EN;
    else if (arg[0] == 'z' && arg[1] == 'h' && (arg[2] == 0 || arg[2] == ' ')) l = LANG_ZH;
    else { vga_write(tr(L_LANG_USAGE)); return; }

    int r = lang_set(l);
    vga_write(tr(L_LANG_SET)); vga_write(lang_name()); vga_write("\n");
    if (r == -2) vga_write(tr(L_LANG_PERSIST_FAIL));
}

static void cmd_sndinfo(void) {
    if (!snd_info(SND_INFO_READY)) {
        vga_write(tr(L_SND_NONE));
        return;
    }
    int be = snd_info(SND_INFO_BACKEND);
    if (be == 2) {
        /* v2.21 #56: PCI backend.  Keep the SB16 output below byte-for-byte
         * unchanged -- tests/snd_regress.py asserts on that text. */
        vga_write("snd: "); vga_write(snd_backend_name()); vga_write(" ready\n");
        vga_write("snd: pci dev=0x"); print_hex((uint32_t)snd_info(SND_INFO_DEVICE));
        vga_write(" iobase=0x");     print_hex((uint32_t)snd_info(SND_INFO_DMASET));
        vga_write(" irq=");          print_int(snd_info(SND_INFO_IRQSET));
        vga_write(" dsp(dev)=0x");  print_hex((uint32_t)snd_info(SND_INFO_VERSION));
        vga_write(" rate=");        print_int(snd_info(SND_INFO_RATE));
        vga_write(" irqs=");        print_int(snd_info(SND_INFO_IRQS));
        vga_write("\n");
        vga_write("snd: pcm buffer "); print_int(snd_info(SND_INFO_BUFSZ));
        vga_write(" bytes (widened to 16-bit stereo in device DMA)\n");
        return;
    }
    vga_write("snd: SB16 ready iobase=");
    print_hex(SND_IOBASE);
    vga_write(" irq=");   print_int(SND_IRQ_LINE);
    vga_write(" dma=");   print_int(SND_DMA8_CHAN);
    vga_write(" dsp=0x"); print_hex((uint32_t)snd_info(SND_INFO_VERSION));
    vga_write(" rate=");  print_int(snd_info(SND_INFO_RATE));
    vga_write(" irqs=");  print_int(snd_info(SND_INFO_IRQS));
    vga_write("\n");
    vga_write("snd: dma buffer ");
    print_int(snd_info(SND_INFO_BUFSZ));
    vga_write(" bytes at 0x"); print_hex(SND_DMA_PHYS);
    vga_write(" mixer irq=");  print_hex((uint32_t)snd_info(SND_INFO_IRQSET));
    vga_write(" dma=");        print_hex((uint32_t)snd_info(SND_INFO_DMASET));
    vga_write("\n");
}

/* Read one unsigned decimal from *pp; leave 0 unchanged when absent. */
static uint32_t snd_parse_u32(const char **pp) {
    const char *p = *pp;
    while (*p == ' ') p++;
    uint32_t v = 0;
    int any = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10u + (uint32_t)(*p - '0');
        p++; any = 1;
    }
    *pp = p;
    return any ? v : 0;
}

static void cmd_sndtest(const char *arg) {
    uint32_t freq = 440u, ms = 800u;
    if (*arg) {
        uint32_t f = snd_parse_u32(&arg);
        uint32_t m = snd_parse_u32(&arg);
        if (f) freq = f;
        if (m) ms = m;
    }
    if (!snd_info(SND_INFO_READY)) {
        vga_write("snd: no SB16 - SNDTEST SKIP\n");
        return;
    }
    int before = snd_info(SND_INFO_IRQS);
    vga_write("snd: playing "); print_int((int)freq);
    vga_write(" Hz for ");     print_int((int)ms);
    vga_write(" ms\n");

    int rc = snd_tone(freq, ms);
    if (rc < 0) {
        vga_write("snd: play failed rc="); print_int(rc);
        vga_write("\nSNDTEST FAIL\n");
        return;
    }
    /* Wait for the DMA-completion IRQ to clear SND_INFO_PLAYING.  `hlt`
     * parks the CPU until the next interrupt; the shell is ring0 with
     * IF=1, so PIT/SB16 interrupts still arrive.  Upper bound 4 s. */
    uint32_t deadline = task_uptime_ms() + 4000u;
    while (snd_info(SND_INFO_PLAYING) && task_uptime_ms() < deadline)
        __asm__ volatile ("hlt");

    int after = snd_info(SND_INFO_IRQS);
    vga_write("snd: irqs="); print_int(after - before);
    vga_write(" busy=");     print_int(snd_info(SND_INFO_PLAYING));
    vga_write("\n");
    /* v2.21 #56: QEMU's ES1370 has no working DAC interrupt (STATUS is
     * read-only there), so completion is a software timeout driven by
     * timer_ticks.  Requiring an IRQ delta would fail an otherwise
     * perfectly played tone.  SB16 keeps the strict IRQ check. */
    int completed = (snd_info(SND_INFO_BACKEND) == 2) ? 1 : (after > before);
    if (completed && !snd_info(SND_INFO_PLAYING))
        vga_write("SNDTEST PASS\n");
    else
        vga_write("SNDTEST FAIL (no DMA completion IRQ)\n");
}

/* ---------- display front-end commands ---------------------------------
 * These commands only talk to render.h.  They never touch a VGA register
 * or a framebuffer, and there is deliberately no pixel-drawing command:
 * graphics mode is a TEXT mode with more colours and styles. */
static int style_name_eq(const char *p, int n, const char *name) {
    int i = 0;
    while (i < n && name[i] && p[i] == name[i]) i++;
    return i == n && name[i] == 0;
}

static void cmd_style(const char *arg) {
    if (!*arg) {
        vga_write("usage: style SPEC   SPEC = decimal mask, 'off', or names\n"
                  "       bold dim italic underline inverse strike hidden\n"
                  "       joined with '+' e.g. style bold+underline\n");
        return;
    }
    uint8_t a = 0;
    const char *p = arg;
    if (*p >= '0' && *p <= '9') {
        while (*p >= '0' && *p <= '9') { a = (uint8_t)(a * 10 + (*p - '0')); p++; }
    } else {
        while (*p) {
            if (*p == '+' || *p == ' ') { p++; continue; }
            const char *s = p;
            while (*p && *p != '+' && *p != ' ') p++;
            int n = (int)(p - s);
            if      (style_name_eq(s, n, "off"))      a = 0;
            else if (style_name_eq(s, n, "none"))     a = 0;
            else if (style_name_eq(s, n, "bold"))     a |= CON_ATTR_BOLD;
            else if (style_name_eq(s, n, "dim"))      a |= CON_ATTR_DIM;
            else if (style_name_eq(s, n, "italic"))   a |= CON_ATTR_ITALIC;
            else if (style_name_eq(s, n, "underline"))a |= CON_ATTR_UNDERLINE;
            else if (style_name_eq(s, n, "inverse"))  a |= CON_ATTR_INVERSE;
            else if (style_name_eq(s, n, "strike"))   a |= CON_ATTR_STRIKE;
            else if (style_name_eq(s, n, "hidden"))   a |= CON_ATTR_HIDDEN;
            else {
                vga_write("style: unknown token (see 'help')\n");
                return;
            }
        }
    }
    con_set_attrs(a);
    vga_write("style mask = 0x");
    print_hex((uint32_t)a);
    vga_write("\n");
}

static void skip_sp(const char **pp) { while (**pp == ' ') (*pp)++; }

static uint32_t parse_hex(const char **pp) {
    const char *p = *pp;
    uint32_t v = 0;
    while (*p) {
        int d;
        if (*p >= '0' && *p <= '9')      d = *p - '0';
        else if (*p >= 'a' && *p <= 'f') d = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'F') d = *p - 'A' + 10;
        else break;
        v = v * 16 + (uint32_t)d;
        p++;
    }
    *pp = p;
    return v;
}

static void cmd_palette(const char *arg) {
    const char *p = arg;
    skip_sp(&p);
    if (!*p) { vga_write("usage: palette IDX RRGGBB\n"); return; }
    int idx = 0;
    while (*p >= '0' && *p <= '9') { idx = idx * 10 + (*p - '0'); p++; }
    skip_sp(&p);
    uint32_t rgb = parse_hex(&p);
    if (idx < 0 || idx > 255) { vga_write("palette: index must be 0..255\n"); return; }
    con_set_palette((uint8_t)idx, rgb);
    vga_write("palette["); print_int(idx);
    vga_write("] = "); print_hex(rgb); vga_write("\n");
}

static void cmd_rgb(const char *arg) {
    const char *p = arg;
    skip_sp(&p);
    uint32_t fg = parse_hex(&p);
    skip_sp(&p);
    uint32_t bg = *p ? parse_hex(&p) : 0;
    con_set_style_rgb(fg, bg, con_get_attrs());
    vga_write("fg="); print_hex(fg);
    vga_write(" bg="); print_hex(bg); vga_write("\n");
}

static void cmd_vbe(void) {
    int r = con_request_mode(CON_MODE_VBE, CON_ORIGIN_CONSOLE);
    if (r == CON_ENODEV) {
        vga_write("vbe: "); vga_write(tr(L_GFX_NONE));
        vga_write("     (QEMU: -vga std or -vga vmware)\n");
        return;
    }
    if (r != 0) { vga_write("vbe: switch failed\n"); return; }
    con_set_style_rgb(0xFFFFFF, 0x103050, CON_ATTR_BOLD);
    vga_write("  CatOS VBE 800x600x32 - truecolour TEXT terminal\n");
    con_set_style_rgb(0xFFD700, 0x000000, 0);
    vga_write("  中文显示测试：你好，世界！\n");
    con_set_style_rgb(0x7CFC00, 0x000000, CON_ATTR_UNDERLINE);
    vga_write("  16.7M 色，每个字符的颜色都是真正的 RGB\n");
    con_set_style(CON_LIGHT_GREY, CON_BLACK, 0);
    vga_write("  type 'text' + confirm Y to leave\n\n");
}

/* v2.12: switch to the 24bpp back-end and paint the fixed diagnostic
 * picture.  Order matters: con_request_mode() repaints the whole cell
 * grid, and vga_clear() repaints it again, so the demo MUST come last.
 * Only the shell's top-row prompt is allowed to overlay it afterwards --
 * every pixel sampled by tests/gfx24_regress.py sits at y >= 120. */
static void size_restore(void);

static void cmd_gfx24(void) {
    int r = con_request_mode(CON_MODE_VBE24, CON_ORIGIN_CONSOLE);
    if (r == CON_ENODEV) {
        vga_write("gfx24: "); vga_write(tr(L_GFX_NONE));
        vga_write("       (QEMU: -vga std or -vga vmware)\n");
        return;
    }
    if (r != 0) { vga_write("gfx24: switch failed\n"); return; }
    vga_clear();                 /* clears cells + resets cursor to (0,0) */
    size_restore();              /* v2.25: re-apply /etc/size if present */
    gfx24_demo();
}

static void cmd_gfx(void) {
    int r = con_request_mode(CON_MODE_GFX, CON_ORIGIN_CONSOLE);
    if (r != 0) { vga_write("gfx: switch failed\n"); return; }
    /* Styled-text welcome.  No rectangles, no pixels, no demo drawing. */
    con_set_style(CON_WHITE, CON_BLUE, CON_ATTR_BOLD);
    vga_write("  CatOS graphics - 40x12 8x16 TEXT cells, 256 colors\n");
    con_set_style_rgb(0xFFD700, 0x000000, CON_ATTR_BOLD);
    vga_write("  中文显示测试：你好，世界！\n");
    con_set_style(CON_YELLOW, CON_BLACK, CON_ATTR_UNDERLINE);
    vga_write("  underline   ");
    con_set_style(CON_LIGHT_CYAN, CON_BLACK, CON_ATTR_ITALIC);
    vga_write("italic   ");
    con_set_style(CON_LIGHT_GREEN, CON_BLACK, CON_ATTR_BOLD);
    vga_write("bold\n");
    con_set_style(CON_WHITE, CON_RED, CON_ATTR_INVERSE);
    vga_write("  inverse  ");
    con_set_style(CON_WHITE, CON_BLACK, CON_ATTR_STRIKE);
    vga_write("strike  ");
    con_set_style(200, CON_BLACK, 0);
    vga_write("grey-200\n");
    con_set_style(CON_LIGHT_GREY, CON_BLACK, 0);
    vga_write("  'text'+Y to leave; 'style', 'rgb', 'palette' to decorate\n\n");
}

static void cmd_text(void) {
    int r = con_request_mode(CON_MODE_TEXT, CON_ORIGIN_CONSOLE);
    if (r == CON_EACCES)      vga_write("text: denied - mode unchanged\n");
    else if (r == CON_ENODEV) vga_write("text: backend unavailable\n");
    else if (r != 0)          vga_write("text: switch failed\n");
    else {
        /* #57: leaving graphics mode restores the 16-colour VGA DAC and
         * palette (be_text::enter does that), but the old graphics-era
         * cells would still be repainted cell-by-cell.  Wipe the screen so
         * the user gets a clean text console instead of a stale picture
         * rendered in text-mode colours. */
        vga_clear();
        con_set_style(CON_LIGHT_GREY, CON_BLACK, 0);
        vga_write("text: back to 80x25 text mode (CJK via dynamic font slots)\n");
    }
}

static void cmd_display(void) {
    vga_write("display backend  : "); vga_write(con_backend_name()); vga_write("\n");
    vga_write("mode             : "); vga_write(con_mode_name(con_get_mode()));
    vga_write("\n");
    vga_write("logical grid     : "); print_int(con_cols());
    vga_write(" x "); print_int(con_rows()); vga_write("\n");
    vga_write("physical pixels  : "); print_int(con_info(CON_INFO_PIXEL_W));
    vga_write(" x "); print_int(con_info(CON_INFO_PIXEL_H)); vga_write("\n");
    vga_write("colours / depth  : "); print_int(con_info(CON_INFO_COLORS));
    vga_write(" / "); print_int(con_info(CON_INFO_BPP)); vga_write(" bpp\n");
    vga_write("style bitmask    : "); print_hex((uint32_t)con_info(CON_INFO_ATTRS));
    vga_write("\nCJK support      : ");
    vga_write(con_info(CON_INFO_CJK) ? "yes" : "no (degrades to '?')");
    vga_write("\nGB2312 glyphs    : "); print_int(con_info(CON_INFO_HZ_COUNT));
    vga_write("\nlogical cell     : "); print_int(con_info(CON_INFO_CELL_W));
    vga_write("x"); print_int(con_info(CON_INFO_CELL_H));
    vga_write(" px (hanzi = 2x1 cells)");
    vga_write("\nanimated blink    : ");
    vga_write(con_info(CON_INFO_BLINK) ? "yes (0.5 s, PIT-driven)" : "no");
    vga_write("\nfast scrolling    : ");
    vga_write(con_info(CON_INFO_SCROLL_FAST) ? "framebuffer move" : "full repaint");
    vga_write("\nvbe accel         : ");
    if (con_get_mode() == CON_MODE_VBE) {
        vga_write(con_info(CON_INFO_WC) ? "MTRR-WC" : "UC(fallback)");
        vga_write(" + ");
        vga_write(con_info(CON_INFO_HW_SCROLL) ? "HW-pan" : "re-render");
    } else {
        vga_write("n/a (only in VBE mode)");
    }
    vga_write("\nIME               : F2 toggles pinyin (shell + edit)");
    vga_write("\nuser drawing API : ");
    if (con_get_mode() == CON_MODE_VBE24)
        vga_write("GFX24 (gfx_info/gfx_rect/gfx_blit, kernel-side writes)\n");
    else
        vga_write("inactive (framebuffers stay supervisor-only)\n");
    vga_write("graphics HW      : ");
    vga_write(svga_probe() ? "VMware SVGA II (15AD:0405)" : "Bochs/QEMU VBE dispi");
    vga_write(" | last gfx24 hw=");
    vga_write(gfx24_info(GFX_INFO_BACKEND) == 1 ? "SVGA-II" : "Bochs");
    vga_write(" hw_bpp="); print_int(gfx24_info(GFX_INFO_HW_BPP));
    vga_write(" stride="); print_int(gfx24_info(GFX_INFO_STRIDE));
    vga_write("\ndirty-cell tracker: ");
    print_int(con_info(CON_INFO_CELLS_DRAWN));
    vga_write(" drawn, ");
    print_int(con_info(CON_INFO_CELLS_SKIPPED));
    vga_write(" skipped (identical repaints cost 0 VRAM writes)\n");
    vga_write("glyph scale       : ");
    print_int(con_info(CON_INFO_CELL_SCALE));
    vga_write(" (0 compact 8x8 / 1 normal 8x16 / 2 large 16x32)\n");
    vga_write("switch policy    : ->gfx/vbe/vbe24 free; ->text needs physical Y\n");
}

/* v2.20: raw SVGA II register dump.  Useful when a VMware guest shows a
 * black screen: `id` must be 0x9000000x, `active` 1 after vbe/gfx24. */
static void cmd_svgainfo(void) {
    if (!svga_probe()) {
        vga_write("svgainfo: no VMware SVGA II device (15AD:0405) on the PCI bus\n");
        vga_write("          QEMU std VGA uses Bochs dispi instead; use 'vdiag'\n");
        return;
    }
    vga_write("SVGA II adapter  : present\n");
    vga_write("id               : 0x"); print_hex(svga_info(SVGA_INFO_ID));
    vga_write("  caps=0x"); print_hex(svga_info(SVGA_INFO_CAPS));
    vga_write("  io=0x"); print_hex(svga_info(SVGA_INFO_IO_BASE));
    vga_write("\nactive           : "); print_int(svga_info(SVGA_INFO_ACTIVE));
    vga_write("  mode "); print_int(svga_info(SVGA_INFO_WIDTH));
    vga_write("x"); print_int(svga_info(SVGA_INFO_HEIGHT));
    vga_write("x"); print_int(svga_info(SVGA_INFO_BPP));
    vga_write("bpp  pitch="); print_int(svga_info(SVGA_INFO_PITCH));
    vga_write("\nframebuffer      : 0x"); print_hex(svga_info(SVGA_INFO_FB_PHYS));
    vga_write(" size="); print_int(svga_info(SVGA_INFO_FB_BYTES));
    vga_write("\nFIFO             : 0x"); print_hex(svga_info(SVGA_INFO_FIFO_PHYS));
    vga_write(" size="); print_int(svga_info(SVGA_INFO_FIFO_BYTES));
    vga_write(" updates="); print_int(svga_info(SVGA_INFO_FLUSHES));
    vga_write("\n");
}

static void cmd_about(void) {
    vga_set_fg(VGA_LIGHT_CYAN);
    vga_write(
        "###          ##    ####   #####\n"
        "##             ##   ##  ## ##\n"
        "##      ####  ###### ##  ## ##\n"
        "#          ##   ##   ##  ##  ####\n"
        "##      #####   ##   ##  ##     ##\n"
        "##    ##  ##   ##   ##  ##     ##\n"
        "###  #####    ###  ####  #####\n"
    );
    vga_set_fg(VGA_LIGHT_GREY);
    vga_write("  A tiny Unix-like OS on GRUB Multiboot\n");
    vga_write("  Architecture : i386 (32-bit x86)\n");
    vga_write("  Bootloader   : GRUB 2 (ISO9660 / El Torito)\n");
    vga_write("  Kernel       : interrupts + paging + tasks\n");
    vga_write("                 ring3 CE apps via int 0x80, uid perms\n");
    vga_write("  Display      : front-end + text/gfx back-ends ('display')\n");
    vga_write("  Input        : PS/2 keyboard (IRQ-driven)\n");
    vga_write("  Filesystem   : FAT32 on IDE (persistent)\n");
    vga_write("  Runs on      : QEMU / VMware / Bochs / real PC\n");
}

static void cmd_uname(void) {
    vga_write("CatOS i386 0.3 (grub-multiboot, 5-pillar kernel)\n");
}

static void cmd_date(void) {
    rtc_time_t t; rtc_read(&t);
    vga_write("Current RTC: ");
    print_pad(t.year, 4); vga_write("-");
    print_pad(t.month, 2); vga_write("-");
    print_pad(t.day, 2); vga_write(" ");
    print_pad(t.hour, 2); vga_write(":");
    print_pad(t.minute, 2); vga_write(":");
    print_pad(t.second, 2);
    vga_write("\n");
}

static void cmd_mem(void) {
    if (mbi && (mbi->flags & 1)) {
        vga_write("Lower memory (0-640K): "); print_int(mbi->mem_lower); vga_write(" KB\n");
        vga_write("Upper memory (>1MB)  : "); print_int(mbi->mem_upper); vga_write(" KB\n");
        vga_write("Total approx         : ");
        print_int((mbi->mem_lower + mbi->mem_upper) / 1024);
        vga_write(" MB\n");
    } else {
        vga_write("Memory info not provided by GRUB.\n");
    }
}

static void cmd_reboot(void) {
    vga_write("Rebooting...\n");
    outb(0x64, 0xFE);
    while (1) __asm__ volatile ("hlt");
}

static void cmd_hang(void) {
    vga_write("Halting. Press Ctrl-Alt-Del or close the VM.\n");
    __asm__ volatile ("cli");
    while (1) __asm__ volatile ("hlt");
}

/* ---------- filesystem commands ---------- */
static void cmd_cd(const char *arg) {
    int r = fs_chdir(arg);
    if (r == FS_ERR_NOENT)      vga_write("cd: no such directory\n");
    else if (r == FS_ERR_NOTDIR) vga_write("cd: not a directory\n");
}

/* v2.24 forward declarations (helpers live across shell.c) */
static int g_uid;         /* session uid/gid (defined with the session code) */
static int g_gid;         /* tentative decls: the real defs sit below */
static int fs_copy_file(const char *src, const char *dst);

/* v2.24: write-permission check on the PARENT of a would-be entry -- the
 * shell is ring0, so nobody else does it.  Without this any user could
 * mkdir/cp straight into /etc or /bin (the ACL default only guards files
 * written through the redirect path). */
static int perm_write_on_parent(const char *path, char *parent, int pcap) {
    char abs[FS_PATH_MAX];
    fs_canon_path(path, abs, sizeof(abs));
    const char *leaf = abs;
    for (const char *q = abs; *q; q++) if (*q == '/') leaf = q + 1;
    if (!*leaf) return -1;
    int plen = (int)(leaf - abs);
    if (plen == 1) { if (pcap > 0) parent[0] = '/'; parent[1] = 0; }
    else {
        if (plen >= pcap) return -1;
        for (int i = 0; i < plen - 1; i++) parent[i] = abs[i];
        parent[plen - 1] = 0;
    }
    return fs_perm_check(parent, g_uid, g_gid, PERM_WRITE) != 0 ? -13 : 0;
}

/* v2.24: mkdir [-p] PATH.  Without -p the parent must already exist;
 * with -p every missing level is created (FS_ERR_EXIST tolerated). */
static void cmd_mkdir_p(const char *arg, int parents) {
    if (!*arg) { vga_write("usage: mkdir [-p] PATH\n"); return; }
    char parent[FS_PATH_MAX];
    if (perm_write_on_parent(arg, parent, sizeof(parent)) == -13) {
        vga_write("mkdir: permission denied\n");
        return;
    }
    if (!parents) {
        int r = fs_mkdir(arg);
        if (r == FS_OK)            return;
        if (r == FS_ERR_NOENT)      vga_write("mkdir: parent directory not found\n");
        else if (r == FS_ERR_NOTDIR) vga_write("mkdir: parent is not a directory\n");
        else if (r == FS_ERR_EXIST) vga_write("mkdir: file or directory already exists\n");
        else if (r == FS_ERR_INVAL) vga_write("mkdir: invalid name\n");
        else if (r == FS_ERR_FULL)  vga_write("mkdir: disk full (FAT32)\n");
        return;
    }
    /* -p: create every missing level, tolerate existing ones */
    char cur[FS_PATH_MAX];
    int ci = 0;
    const char *p2 = arg;
    if (*p2 == '/') { cur[ci++] = '/'; p2++; }
    cur[ci] = 0;
    while (*p2) {
        const char *seg = p2;
        while (*p2 && *p2 != '/') p2++;
        int sl = (int)(p2 - seg);
        if (sl && ci + sl + 1 < (int)sizeof(cur)) {
            for (int k = 0; k < sl; k++) cur[ci++] = seg[k];
            cur[ci] = 0;
            if (fs_is_dir(cur)) { /* already there */ }
            else {
                int r = fs_mkdir(cur);
                if (r != FS_OK && r != FS_ERR_EXIST) {
                    vga_write("mkdir: cannot create '");
                    vga_write(cur);
                    vga_write("' (");
                    print_int(r);
                    vga_write(")\n");
                    return;
                }
            }
        }
        if (*p2 == '/') {
            if (ci + 1 < (int)sizeof(cur)) cur[ci++] = '/';
            cur[ci] = 0;
            p2++;
        }
    }
}

static void cmd_touch(const char *arg) {
    if (!*arg) { vga_write("usage: touch FILE\n"); return; }
    int r = fs_touch(arg);
    if (r == FS_OK)            return;
    if (r == FS_ERR_NOENT)      vga_write("touch: parent directory not found\n");
    else if (r == FS_ERR_NOTDIR) vga_write("touch: is a directory\n");
    else if (r == FS_ERR_INVAL) vga_write("touch: invalid name\n");
    else if (r == FS_ERR_FULL)  vga_write("touch: disk full (FAT32)\n");
}

/* split a string into words (in place); returns word count, max bounded */
static int split_words(char *s, char **out, int max) {
    int n = 0;
    while (*s && n < max) {
        while (*s == ' ') s++;
        if (!*s) break;
        out[n++] = s;
        while (*s && *s != ' ') s++;
        if (*s) { *s = 0; s++; }
    }
    return n;
}

/* bounded concatenation: never writes past d+size; returns 0 if it fit */
static int strcat_b(char *d, int size, const char *s) {
    while (*d && size > 0) { d++; size--; }
    if (size <= 1) return -1;
    while (*s && size > 1) { *d++ = *s++; size--; }
    *d = 0;
    return *s ? -1 : 0;
}

/* v2.23 environment variables (defined near the shell loop below) */
static const char *env_get_(const char *key);

/* run "cmd" as a CE program: current directory first, then /bin (PATH).
 * Returns 0 when a program was found (spawn attempted), -1 if not found.
 * A command that already carries an extension (e.g. "hello.elf") is tried
 * verbatim -- ".ce" is appended only when the last path component has no
 * dot. */
static int cmd_has_ext(const char *cmd) {
    const char *p = cmd, *base = cmd;
    while (*p) {
        if (*p == '/') base = p + 1;
        else if (*p == '.') return 1;
        p++;
    }
    (void)base;
    return 0;
}

static int run_from_path(const char *cmd, char **argv, int argc, int bg, int uid) {
    if (!cmd || !*cmd) return -1;
    if (!fs_fs_ready()) return -1;
    char p[FS_PATH_MAX];
    char pwd[FS_PATH_MAX];
    fs_pwd(pwd, sizeof(pwd));
    uint32_t c = 0, s = 0;
    const char *ext = cmd_has_ext(cmd) ? "" : ".ce";

    /* 1) current directory (bounded: never overflow like the old code) */
    p[0] = 0;
    if (!(pwd[0] == '/' && pwd[1] == 0)) {
        if (strcat_b(p, sizeof(p), pwd) != 0) return -1;
        if (strcat_b(p, sizeof(p), "/") != 0) return -1;
    } else {
        strcat_b(p, sizeof(p), "/");
    }
    if (strcat_b(p, sizeof(p), cmd) != 0) return -1;
    if (strcat_b(p, sizeof(p), ext) != 0) return -1;
    if (fs_resolve_file(p, &c, &s) == 0) {
        spawn_and_maybe_wait(p, argc, argv, bg, uid);
        return 0;
    }
    /* 2) $PATH lookup (v2.23; default "/bin", colon-separated, in order) */
    const char *pathv = env_get_("PATH");
    if (!pathv || !*pathv) pathv = "/bin";
    const char *seg = pathv;
    while (*seg) {
        char dir[64];
        int di = 0;
        while (*seg && *seg != ':' && di < (int)sizeof(dir) - 1)
            dir[di++] = *seg++;
        dir[di] = 0;
        if (*seg == ':') seg++;
        if (!di) continue;
        p[0] = 0;
        if (strcat_b(p, sizeof(p), dir) != 0) continue;
        if (dir[di - 1] != '/' && strcat_b(p, sizeof(p), "/") != 0) continue;
        if (strcat_b(p, sizeof(p), cmd) != 0) continue;
        if (strcat_b(p, sizeof(p), ext) != 0) continue;
        if (fs_resolve_file(p, &c, &s) == 0) {
            spawn_and_maybe_wait(p, argc, argv, bg, uid);
            return 0;
        }
    }
    return -1;
}

/* spawn a .ce as a ring3 task; foreground runs wait for it to exit,
 * background ('&') returns to the shell immediately */
void spawn_and_maybe_wait(const char *path, int argc, char **argv, int bg, int uid) {
    int tid = task_spawn_ce(path, argc, argv, uid);
    if (tid < 0) {
        vga_write("run: cannot load '");
        vga_write(path);
        vga_write("' (");
        print_int(tid);
        vga_write(")\n");
        return;
    }
    if (bg) {
        task_t *bt = task_by_tid(tid);
        if (bt) bt->bg_mode = 1;      /* #59: capture stdout into the ring */
        vga_write("[bg tid "); print_int(tid); vga_write("] ");
        vga_write(path); vga_write("\n");
        return;
    }
    task_wait_child(task_by_tid(tid));
}

/* ---- v2.5: shell redirection (| / > / >>) ----------------------------- */

/* split "cmd args [> F | >> F | | CMD]" into out_redir / pipe_to and
 * truncate arg at the operator.  Returns 1 when a redirection exists. */
static int parse_redir(char *arg, char **out_redir, int *append, char **pipe_to) {
    *out_redir = 0; *append = 0; *pipe_to = 0;
    for (char *q = arg; *q; q++) {
        if (*q == '|') { *pipe_to = q + 1; *q = 0; break; }
        if (*q == '>') {
            if (*(q + 1) == '>') { *append = 1; *out_redir = q + 2; *(q + 1) = 0; }
            else                 { *out_redir = q + 1; }
            *q = 0;
            break;
        }
    }
    if (*out_redir) while (**out_redir == ' ') (*out_redir)++;
    if (*pipe_to)   while (**pipe_to == ' ')   (*pipe_to)++;
    return (*out_redir || *pipe_to) ? 1 : 0;
}

/* make a redirection target absolute against the current directory */
static int redir_abspath(const char *name, char *out, int outlen) {
    if (!name || !*name) return -1;
    out[0] = 0;
    if (name[0] == '/') return strcat_b(out, outlen, name) == 0 ? 0 : -1;
    char pwd[FS_PATH_MAX];
    fs_pwd(pwd, sizeof(pwd));
    if (pwd[0] == '/' && pwd[1] == 0) {
        if (strcat_b(out, outlen, "/") != 0) return -1;
    } else {
        if (strcat_b(out, outlen, pwd) != 0) return -1;
        if (strcat_b(out, outlen, "/") != 0) return -1;
    }
    return strcat_b(out, outlen, name) == 0 ? 0 : -1;
}

/* spawn a .ce and wire optional redirection: out_file (append flag) and/or
 * pipe ends.  fg = 1 waits for exit (and the shell then reaps the task). */
static void spawn_ce_redirect(const char *path, int argc, char **argv,
                              int fg, int uid,
                              const char *out_file, int append,
                              int out_pipe, int in_pipe) {
    int tid = task_spawn_ce(path, argc, argv, uid);
    if (tid < 0) {
        vga_write("run: cannot load '");
        vga_write(path);
        vga_write("' (err ");
        print_int(tid);
        vga_write(")\n");
        return;
    }
    task_t *tt = task_by_tid(tid);
    if (out_file && out_file[0]) {
        char ab[FS_PATH_MAX];
        if (redir_abspath(out_file, ab, sizeof(ab)) != 0) {
            vga_write("run: bad redirect path\n");
            task_kill(tid, UID_ROOT);
            return;
        }
        int i;
        for (i = 0; ab[i] && i < (int)sizeof(tt->out_file) - 1; i++)
            tt->out_file[i] = ab[i];
        tt->out_file[i] = 0;
        /* v2.5.1 (FAIL-5-1 fix): `>` truncates once up front, then every
         * SYS_WRITE must append so multi-line output is not clobbered. */
        tt->out_append = 1;
        if (!append)
            fs_write_raw_mode(ab, "", 0, 0);      /* pre-create/truncate; #49: 无条件截断 */
    }
    if (out_pipe >= 0) kpipe_assign_out(tid, out_pipe);
    if (in_pipe  >= 0) kpipe_assign_in(tid, in_pipe);
    if (!fg) {
        tt->bg_mode = 1;              /* #59: capture stdout into the ring */
        vga_write("[bg tid "); print_int(tid); vga_write("] ");
        vga_write(path); vga_write("\n");
        return;
    }
    task_wait_child(tt);
}

static void vga_clear_disabled(void) { }
static void cmd_run(char *arg, int uid) {
    while (*arg == ' ') arg++;
    if (!*arg) {
        vga_write("usage: run FILE.ce [args...] [&] [> F] [>> F] [| CMD]\n");
        return;
    }
    if (!fs_fs_ready()) { vga_write("run: no FAT32 volume.\n"); return; }

    char *out_redir = 0, *pipe_to = 0; int append = 0;
    parse_redir(arg, &out_redir, &append, &pipe_to);

    /* split FILE.ce from its arguments */
    char *file = arg;
    while (*arg && *arg != ' ') arg++;
    if (*arg) { *arg = 0; arg++; }
    char *argv[16];
    int argc = 1;
    argv[0] = file;
    argc += split_words(arg, argv + 1, 15);
    int bg = 0;
    if (argc > 1 && argv[argc - 1][0] == '&' && argv[argc - 1][1] == 0) {
        bg = 1;
        argc--;
    }

    if (pipe_to && *pipe_to) {
        /* a | b : producer (background, writes the pipe) then consumer
         * (foreground, reads the pipe until EOF when the writer exits) */
        char *file2 = pipe_to;
        char *sp = pipe_to;
        while (*sp && *sp != ' ') sp++;
        if (*sp) { *sp = 0; sp++; }
        char *argv2[16];
        int argc2 = 1;
        argv2[0] = file2;
        argc2 += split_words(sp, argv2 + 1, 15);
        int h = kpipe_create();
        if (h < 0) { vga_write("run: no pipe slots\n"); return; }
        vga_clear();
        spawn_ce_redirect(file, argc, argv, 0, uid, 0, 0, h, -1);
        spawn_ce_redirect(file2, argc2, argv2, 1, uid, 0, 0, -1, h);
        return;
    }
    vga_clear_disabled();
    spawn_ce_redirect(file, argc, argv, !bg, uid, out_redir, append, -1, -1);
}

/* ---------- netcfg: view/change IP config (VMware NAT etc.) ---------- */
static int ip4_parse_(const char *s, uint8_t out[4]) {
    int v = 0, n = 0;
    for (int i = 0; i < 4; i++) out[i] = 0;
    for (const char *q = s; *q; q++) {
        if (*q >= '0' && *q <= '9') { v = v * 10 + (*q - '0'); if (v > 255) return 0; }
        else if (*q == '.') { if (n >= 3) return 0; out[n++] = (uint8_t)v; v = 0; }
        else return 0;
    }
    out[n++] = (uint8_t)v;
    return n == 4;
}

static void cmd_netcfg(char *arg) {
    extern uint8_t net_our_ip[4];
    extern uint8_t net_gw_ip[4];
    extern void net_dns_server_set(const uint8_t ip[4]);
    extern const uint8_t *net_dns_server_get(void);
    extern void net_arp_flush(void);

    if (!*arg) {
        vga_write("ip  : ");
        for (int i = 0; i < 4; i++) { print_int(net_our_ip[i]); if (i < 3) vga_write("."); }
        vga_write("  gw: ");
        for (int i = 0; i < 4; i++) { print_int(net_gw_ip[i]); if (i < 3) vga_write("."); }
        vga_write("  dns: ");
        const uint8_t *d = net_dns_server_get();
        for (int i = 0; i < 4; i++) { print_int(d[i]); if (i < 3) vga_write("."); }
        vga_write("\nusage: netcfg IP GATEWAY [DNS]");
        vga_write("   e.g. netcfg 192.168.152.8 192.168.152.1 192.168.152.2\n");
        return;
    }
    uint8_t ip[4], gw[4], dns[4];
    char *w[4];
    int n = 0;
    w[n++] = arg;
    for (char *q = arg; *q && n < 4; q++)
        if (*q == ' ') { *q = 0; while (q[1] == ' ') q++; w[n++] = q + 1; }
    if (n < 2 || !ip4_parse_(w[0], ip) || !ip4_parse_(w[1], gw)) {
        vga_write("netcfg: bad arguments\n");
        return;
    }
    for (int i = 0; i < 4; i++) net_our_ip[i] = ip[i];
    for (int i = 0; i < 4; i++) net_gw_ip[i] = gw[i];
    if (n >= 3 && ip4_parse_(w[2], dns)) net_dns_server_set(dns);
    net_arp_flush();
    vga_write("netcfg: ip/gw/dns updated, ARP cache cleared\n");
}

/* ---------- hwinfo: full hardware-properties dump ---------- */
static void cmd_hwinfo(void) {
    static char buf[3072];
    int n = hw_summary(buf, sizeof(buf));
    if (n <= 0) { vga_write("hwinfo: nothing\n"); return; }
    if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
    buf[n] = 0;
    vga_write(buf);
}

/* ---------- top: task monitor with live CPU% ---------- */
static void top_draw(uint32_t win_ticks) {
    struct toprow {
        int tid, uid, ring, state;
        uint32_t dt, pages;
        char name[16];
    } rows[MAX_TASKS];
    int nr = 0;
    uint32_t idle_now = task_idle_ticks();

    static uint32_t prev_ticks[MAX_TASKS], prev_idle;
    uint32_t now[MAX_TASKS];

    for (int slot = 0; slot < MAX_TASKS; slot++) {
        int tid; uint32_t ck, pg; int st, uid, ring; char nm[16];
        now[slot] = 0;
        if (!task_stat(slot, &tid, &ck, &pg, &st, &uid, &ring, nm, sizeof(nm)))
            continue;
        now[slot] = ck;
        rows[nr].tid = tid; rows[nr].uid = uid; rows[nr].ring = ring;
        rows[nr].state = st; rows[nr].pages = pg;
        rows[nr].dt = (ck >= prev_ticks[slot]) ? ck - prev_ticks[slot] : 0;
        {
            int i;
            for (i = 0; i < 15 && nm[i]; i++) rows[nr].name[i] = nm[i];
            rows[nr].name[i] = 0;
        }
        nr++;
    }

    /* insertion sort by delta desc */
    for (int i = 1; i < nr; i++) {
        struct toprow r = rows[i];
        int j = i - 1;
        while (j >= 0 && rows[j].dt < r.dt) { rows[j + 1] = rows[j]; j--; }
        rows[j + 1] = r;
    }

    vga_clear();
    vga_write("CatOS top - ");
    print_int((int)(task_uptime_ms() / 1000));
    vga_write("s up, ");
    print_int(hw_num(HW_CPU_MHZ));
    vga_write(" MHz, tasks ");
    print_int(hw_num(HW_TASKS));
    vga_write("\nmem ");
    print_int(hw_num(HW_MEM_USED_KB));
    vga_write("K used / ");
    print_int(hw_num(HW_MEM_FREE_KB));
    vga_write("K free / ");
    print_int(hw_num(HW_MEM_TOTAL_KB));
    vga_write("K total\n");

    if (win_ticks) {
        uint32_t idle_d = (idle_now >= prev_idle) ? idle_now - prev_idle : 0;
        int busy = 100 - (int)(idle_d * 100 / win_ticks);
        if (busy < 0) busy = 0;
        if (busy > 100) busy = 100;
        vga_write("cpu: ");
        print_int(busy);
        vga_write("% busy (");
        print_int((int)idle_d);
        vga_write("/");
        print_int((int)win_ticks);
        vga_write(" idle ticks)\n");
    } else {
        vga_write("cpu: (sampling, next frame in 2s)\n");
    }
    vga_write("TID  UID   RING  STATE   CPU%  PAGES  NAME\n");

    static const char *stn[] = {
        "UNUSED", "READY ", "RUN   ", "SLEEP ", "KB    ", "CHILD ", "ZOMBIE",
    };
    for (int i = 0; i < nr; i++) {
        print_int(rows[i].tid); vga_write(rows[i].tid < 10 ? "    " : "   ");
        print_int(rows[i].uid); vga_write(rows[i].uid < 10 ? "    " : "   ");
        vga_write(rows[i].ring == 0 ? "0    " : "3    ");
        vga_write(stn[rows[i].state <= 6 ? rows[i].state : 0]);
        vga_write(" ");
        if (win_ticks) {
            int pct = (int)(rows[i].dt * 100 / win_ticks);
            if (pct > 100) pct = 100;
            print_pad(pct, 3);
        } else {
            vga_write("  .");
        }
        vga_write("%  ");
        print_pad((int)rows[i].pages, 4);
        vga_write("  ");
        vga_write(rows[i].name);
        vga_write("\n");
    }
    vga_write("-- any key quits, refresh 2s --\n");

    for (int slot = 0; slot < MAX_TASKS; slot++) prev_ticks[slot] = now[slot];
    prev_idle = idle_now;
}

static void cmd_top(void) {
    top_draw(0);                             /* first frame: totals only */
    for (;;) {
        /* wait 2 s (20 ticks) or until a REAL key arrives; stale break
         * codes left in the queue by the launching readline are decoded
         * away and never count as the quit key */
        int k = kb_getkey_timed(20);
        if (k >= 0) break;
        top_draw(20);                        /* 20-tick sampling window */
    }
    vga_clear();
}

/* ---------- #59: jobs / log -- background job output ---------- */
static const char *job_state_name(int st) {
    switch (st) {
    case T_READY:      return "ready";
    case T_RUNNING:    return "run";
    case T_SLEEPING:   return "sleep";
    case T_WAIT_KB:    return "waitkey";
    case T_WAIT_CHILD: return "wait";
    case T_ZOMBIE:     return "done";
    default:           return "?";
    }
}

static void cmd_jobs(void) {
    int tid, state, uid, ring, found = 0;
    uint32_t cpu, pages;
    char nm[16];
    vga_write("TID  STATE    NAME             LOG\n");
    for (int slot = 1; slot < MAX_TASKS; slot++) {
        if (!task_stat(slot, &tid, &cpu, &pages, &state, &uid, &ring,
                       nm, sizeof(nm))) continue;
        task_t *t = task_by_tid(tid);
        if (!t || !t->bg_mode) continue;     /* background jobs only */
        found = 1;
        vga_write("  "); print_int(tid);
        vga_write("   "); vga_write(job_state_name(state));
        vga_write("    "); vga_write(nm);
        vga_write("  "); print_int(task_log_pending(t));
        vga_write("B\n");
    }
    if (!found) vga_write(tr(L_JOBS_NONE));
}

/* log TID      : dump everything the job has printed since the last drain
 * log -f TID   : keep following until the job exits (like tail -f)
 * Both drain the per-task ring, so a second `log TID` shows only new text. */
static void cmd_log(char *arg) {
    while (*arg == ' ') arg++;
    int follow = 0;
    if (arg[0] == '-' && arg[1] == 'f' && (arg[2] == ' ' || arg[2] == 0)) {
        follow = 1; arg += 2;
        while (*arg == ' ') arg++;
    }
    if (*arg < '0' || *arg > '9') {
        vga_write("usage: log [-f] TID   (see `jobs` for TIDs)\n");
        return;
    }
    int tid = 0;
    while (*arg >= '0' && *arg <= '9') tid = tid * 10 + (*arg++ - '0');
    task_t *t = task_by_tid(tid);
    if (!t || t->state == T_UNUSED) { vga_write(tr(L_LOG_NOJOB)); return; }
    if (!t->bg_mode) {
        vga_write("log: task "); print_int(tid);
        vga_write(" is not a background job (its output went to the console)\n");
        return;
    }
    char buf[161];
    int idle = 0, printed = 0;
    for (;;) {
        uint32_t lost = 0;
        int n = task_log_drain(t, buf, 160, &lost);
        if (n > 0) {
            vga_write_n(buf, n, 1);          /* keeps UTF-8 state across chunks */
            printed += n;
            idle = 0;
            continue;
        }
        if (lost) {
            vga_write("\n[log: "); print_int(lost);
            vga_write(" bytes dropped (ring overflow)]\n");
            lost = 0;
        }
        if (!follow) break;
        t = task_by_tid(tid);
        if (!t || t->state == T_UNUSED || t->state == T_ZOMBIE) break;
        if (idle++ > 600) break;             /* ~60 s safety stop */
        task_sleep_ticks(1);                 /* 100 ms */
    }
    if (printed) vga_putc('\n');
}
extern uint8_t net_mac[6];
extern int net_ready;
extern int netdbg_tx_calls, netdbg_tx_last, netdbg_rx_frames, netdbg_io_base;
extern int netdbg_io_width;
extern int      netdbg_pci_count;
extern uint16_t netdbg_pci_vend[8];
extern uint16_t netdbg_pci_dev[8];
extern uint16_t netdbg_csr0;
extern uint32_t netdbg_tx_word0, netdbg_rx_word0;
extern uint32_t netdbg_desc_tx0(void), netdbg_desc_rx0(void);
extern uint32_t netdbg_init_addr(void), netdbg_init_rxring(void), netdbg_init_txring(void);
extern uint16_t netdbg_csr(int i);
extern int net_fail_code;
extern int net_env_vmware(void);
static void cmd_netdiag(const char *arg) {
    (void)arg;
    vga_write("PCI bus 0: ");
    if (netdbg_pci_count == 0) vga_write("no devices\n");
    for (int i = 0; i < netdbg_pci_count; i++) {
        vga_write("vend="); print_hex(netdbg_pci_vend[i]);
        vga_write(" dev="); print_hex(netdbg_pci_dev[i]);
        if (i < netdbg_pci_count - 1) vga_write("  ");
    }
    vga_write("\n");
    vga_write("net_ready="); print_int(net_ready); vga_write("  io_base=0x");
    print_hex((uint32_t)netdbg_io_base);
    vga_write("  width="); print_int(netdbg_io_width); vga_write("-bit\n");
    vga_write("backend="); vga_write(net_backend_name());
    if (e1000_up()) {
        vga_write("  e1000 up="); print_int(e1000_up());
        vga_write("  tx="); print_int(e1000_info(E1000_INFO_TX_FRAMES));
        vga_write("  rx="); print_int(e1000_info(E1000_INFO_RX_FRAMES));
        vga_write("  irq="); print_int(e1000_irq_line());
        vga_write("\n");
    } else {
        vga_write("\n");
    }
    vga_write("fail="); print_int(net_fail_code);
    vga_write(" (0=ok 1=no-dev 2=bus-FF 3=mac 4=init)  env=");
    vga_write(net_env_vmware() ? "VMware" : "QEMU/PC");
    vga_write("  mac_state=");
    if (net_mac[0]==0xFF && net_mac[1]==0xFF && net_mac[2]==0xFF &&
        net_mac[3]==0xFF && net_mac[4]==0xFF && net_mac[5]==0xFF)
        vga_write("OPEN-BUS");
    else vga_write("valid");
    vga_write("\n");
    vga_write("csr0=0x"); print_hex(netdbg_csr0); vga_write("\n");
    vga_write("mac="); print_hex((net_mac[0]<<8)|net_mac[1]); vga_write(":");
    print_hex((net_mac[2]<<8)|net_mac[3]); vga_write(":");
    print_hex((net_mac[4]<<8)|net_mac[5]); vga_write("\n");
    vga_write("tx_calls="); print_int(netdbg_tx_calls);
    vga_write(" tx_last="); print_int(netdbg_tx_last);
    vga_write(" rx_frames="); print_int(netdbg_rx_frames); vga_write("\n");
    vga_write("tx_word0=0x"); print_hex(netdbg_tx_word0);
    vga_write(" rx_word0=0x"); print_hex(netdbg_rx_word0); vga_write("\n");
    vga_write("desc_tx0=0x"); print_hex(netdbg_desc_tx0());
    vga_write(" desc_rx0=0x"); print_hex(netdbg_desc_rx0()); vga_write("\n");
    vga_write("init@0x"); print_hex(netdbg_init_addr());
    vga_write(" rxr=0x"); print_hex(netdbg_init_rxring());
    vga_write(" txr=0x"); print_hex(netdbg_init_txring()); vga_write("\n");
    vga_write("XMTRC="); print_int(netdbg_csr(40));
    vga_write(" CXDA=0x"); print_hex(netdbg_csr(34) | ((uint32_t)netdbg_csr(35) << 16));
    vga_write(" RL/TL="); print_int(netdbg_csr(76)); vga_write("/"); print_int(netdbg_csr(78)); vga_write("\n");
    vga_write("csr0_now=0x"); print_hex(netdbg_csr(0));
    vga_write(" csr15(mode)="); print_hex(netdbg_csr(15));
    vga_write(" POLL="); print_int(netdbg_csr(46)); vga_write("\n");
    vga_write("IADR=0x"); print_hex(netdbg_csr(1) | ((uint32_t)netdbg_csr(2) << 16));
    vga_write(" csr6="); print_hex(netdbg_csr(6));
    vga_write(" XMTRC="); print_int(netdbg_csr(74)); vga_write("\n");
    vga_write("init_rcvrl="); print_int(netdbg_init_rcvrl);
    vga_write(" init_xmtrl="); print_int(netdbg_init_xmtrl); vga_write("\n");
}

/* explain why the network is down, appended to "network not ready" */
static void net_why_down(void) {
    extern int netdbg_pci_count;
    extern uint16_t netdbg_pci_vend[8];
    extern uint16_t netdbg_pci_dev[8];
    extern int net_fail_code;
    extern int net_env_vmware(void);
    if (netdbg_pci_count == 0) {
        vga_write(" (no PCI devices at all)");
    } else {
        switch (net_fail_code) {
        case 1: vga_write(" (no PCnet 1022:2000 on any bus)"); break;
        case 2: vga_write(" (NIC registers read open-bus FF/00)"); break;
        case 3: vga_write(" (MAC unreadable: device not answering)"); break;
        case 4: vga_write(" (INIT/START did not latch)"); break;
        default: vga_write(" (driver not initialised)"); break;
        }
    }
    if (net_env_vmware())
        vga_write("; VMware guest -> set the VM network adapter to Intel E1000\n");
    else
        vga_write("; QEMU -> -device e1000,netdev=n0 or -device pcnet,netdev=n0\n");
}

/* ---------- curl: one-shot HTTP GET ---------- */
static void cmd_curl(const char *arg) {
    while (*arg == ' ') arg++;
    if (!*arg) {
        vga_write("usage: curl http://IP[:port]/path\n");
        vga_write("e.g.   curl http://10.0.2.2:8080/API.txt\n");
        return;
    }
    if (!net_up()) { vga_write("curl: network not ready"); net_why_down(); return; }
    static char buf[8192];
    int n = net_http_get(arg, buf, sizeof(buf));
    if (n < 0) {
        vga_write("curl: error ");
        print_int(n);
        vga_write(" (1=NIC 2=host 3=connect 4=http 5=resp 6=send)\n");
        return;
    }
    vga_write("--- ");
    vga_write(arg);
    vga_write(" ---\n");
    vga_write(buf);
    if (n > 0 && buf[n-1] != '\n') vga_write("\n");
    vga_write("--- "); print_int(n); vga_write(" bytes ---\n");
}

/* ---------- ping: ICMP echo ---------- */
static void cmd_ping(const char *arg) {
    while (*arg == ' ') arg++;
    if (!*arg) {
        vga_write("usage: ping IP_or_host\n");
        return;
    }
    if (!net_up()) { vga_write("ping: network not ready"); net_why_down(); return; }
    for (int i = 0; i < 3; i++) {
        int ok = net_ping(arg);
        vga_write(ok == 0 ? "reply  " : "no-reply ");
        vga_write(arg);
        vga_write("\n");
        if (ok != 0) break;
        for (volatile long d = 0; d < 800000L; d++) { }  /* small gap between tries */
    }
}

/* ---------- dns: resolve a hostname ---------- */
static void cmd_dns(const char *arg) {
    while (*arg == ' ') arg++;
    if (!*arg) { vga_write("usage: dns hostname\n"); return; }
    if (!net_up()) { vga_write("dns: network not ready"); net_why_down(); return; }
    uint8_t ip[4];
    if (net_dns_lookup(arg, ip) != 0) {
        vga_write("dns: could not resolve '");
        vga_write(arg);
        vga_write("'\n");
        return;
    }
    vga_write(arg);
    vga_write(" -> ");
    char t[4];
    for (int i = 0; i < 4; i++) {
        int d = ip[i], ti = 0;
        if (d == 0) { vga_putc('0'); }
        while (d > 0) { t[ti++] = (char)('0' + d % 10); d /= 10; }
        while (ti > 0) vga_putc(t[--ti]);
        if (i < 3) vga_putc('.');
    }
    vga_write("\n");
}

/* ---------- udp: send a datagram, then wait for one reply ----------
 * works with a host echo server, e.g.
 *   python -c "import socket; s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); s.bind(('',9999));
 *              while 1: d,a=s.recvfrom(2048); s.sendto(d,a)"
 */
static void cmd_udp(const char *arg) {
    while (*arg == ' ') arg++;
    char ip[64];
    int pi = 0;
    while (*arg && *arg != ' ' && pi < 63) ip[pi++] = *arg++;
    ip[pi] = 0;
    while (*arg == ' ') arg++;
    int port = 0;
    while (*arg >= '0' && *arg <= '9') { port = port * 10 + (*arg - '0'); arg++; }
    while (*arg == ' ') arg++;
    if (!*ip || port <= 0 || port > 65535 || !*arg) {
        vga_write("usage: udp IP port text\n");
        return;
    }
    if (!net_up()) { vga_write("udp: network not ready"); net_why_down(); return; }
    uint32_t tlen = 0;
    while (arg[tlen]) tlen++;
    if (net_udp_send(ip, (uint16_t)port, (const uint8_t *)arg, tlen) != 0) {
        vga_write("udp: send failed\n");
        return;
    }
    vga_write("sent, waiting for reply...\n");
    static char rbuf[2048];
    char rip[24];
    int rport = 0;
    /* poll the NIC while waiting (the old loop never did -> "no reply") */
    extern volatile uint32_t timer_ticks;
    extern void net_poll(void);
    uint32_t deadline = timer_ticks + 200;
    for (;;) {
        net_poll();
        int n = net_udp_recv(rip, &rport, (uint8_t *)rbuf, sizeof(rbuf) - 1);
        if (n > 0) {
            vga_write("reply from ");
            vga_write(rip);
            vga_write(":");
            print_int(rport);
            vga_write(" (");
            print_int(n);
            vga_write(" bytes): ");
            rbuf[n] = 0;
            vga_write(rbuf);
            vga_write("\n");
            return;
        }
        if ((int32_t)(timer_ticks - deadline) >= 0) break;
        task_maybe_yield();
        task_sleep_ticks(1);
    }
    vga_write("udp: no reply\n");
}

static void cmd_format(void) {    vga_write(tr(L_FORMAT_CONFIRM));
    vga_write("Disk: ");
    uint32_t tot = 0;
    ata_identify(&tot);
    if (tot == 0) tot = 131072;
    print_int((int)(tot / 2048));            /* MiB */
    vga_write(" MiB IDE, FAT32, 512B/sector ");
    uint32_t cs = fat_cluster_size();
    print_int((int)(cs ? cs / 512 : 8));
    vga_write(" sectors/cluster.\n");
    vga_write("Proceed? (y/n) ");
    char ans[16];
    kb_readline(ans, sizeof(ans));
    if (ans[0] == 'y' || ans[0] == 'Y') {
        if (fs_format() != 0) {
            vga_write("format: no IDE disk attached\n");
            return;
        }
        vga_write(tr(L_FORMAT_DONE));
        vga_write("Top-level dirs: /dev /etc /bin /sbin /lib /usr /home\n");
        vga_write("                /root /tmp /var /proc /sys /mnt /boot\n");
        ce_install_builtins();      /* put /bin apps + /appdevelop docs on disk */
    } else {
        vga_write(tr(L_FORMAT_CANCEL));
    }
}

/* ---------- nano-style full-screen editor ---------- */
#define ED_MAX_LINES 64
#define ED_MAX_COLS  99          /* widest terminal (VBE) is 100 columns */
#define ED_MAX_VIEW  60

static char ed_lines[ED_MAX_LINES][ED_MAX_COLS + 1];
static int  ed_nlines;
static int  ed_modified;
static int  ed_top;              /* first visible content line */
static int  ed_cx, ed_cy;        /* cursor column / content line */
static char ed_fname[FS_NAME_MAX];

static int ed_strlen(const char *s) { int n = 0; while (s[n]) n++; return n; }

/* v2.9: the editor follows the ACTIVE back-end's grid instead of being
 * hard-wired to 80x25.  Row 0 is the title, row con_rows()-1 the bars. */
static int ed_view_rows(void) {
    int r = con_rows() - 2;
    if (r < 1) r = 1;
    if (r > ED_MAX_VIEW) r = ED_MAX_VIEW;
    return r;
}
static int ed_max_cols(void) {
    int c = con_cols() - 1;
    if (c < 1) c = 1;
    if (c > ED_MAX_COLS) c = ED_MAX_COLS;
    return c;
}

static void ed_status_msg(const char *m) {
    int row = con_rows() - 1;
    if (row < 1) row = 1;
    vga_set_color(VGA_WHITE, VGA_LIGHT_BLUE);
    for (int x = 0; x < con_cols(); x++) vga_putc_at((uint8_t)x, (uint8_t)row, ' ');
    vga_write_at_utf8(0, (uint8_t)row, m);
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
}

static void ed_load(fs_entry_t *f) {
    ed_nlines = 0;
    int n = fs_line_count(f);
    char lb[ED_MAX_COLS + 2];
    for (int i = 0; i < n && i < ED_MAX_LINES; i++) {
        if (fs_get_line(f, i, lb, sizeof(lb)) == 0) {
            int l = ed_strlen(lb);
            if (l > ED_MAX_COLS) l = ED_MAX_COLS;
            for (int j = 0; j < l; j++) ed_lines[ed_nlines][j] = lb[j];
            ed_lines[ed_nlines][l] = 0;
            ed_nlines++;
        }
    }
    if (ed_nlines == 0) { ed_lines[0][0] = 0; ed_nlines = 1; }
    ed_modified = 0;
    ed_top = 0; ed_cx = 0; ed_cy = 0;
}

static int ed_save(fs_entry_t *f) {
    int total = 0;
    for (int i = 0; i < ed_nlines; i++) total += ed_strlen(ed_lines[i]) + 1;
    if (total > FS_DATA_MAX) return -1;
    fs_clear_file(f);
    for (int i = 0; i < ed_nlines; i++)
        if (fs_append_line(f, ed_lines[i]) != 0) return -1;
    if (fs_save(f) != 0) return -1;    /* write back to the FAT32 volume */
    ed_modified = 0;
    return 0;
}

static void ed_redraw(void) {
    int sc = con_cols();
    int last = con_rows() - 1;
    if (last < 2) last = 2;
    int view = ed_view_rows();

    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    for (int y = 0; y <= last; y++)
        for (int x = 0; x < sc; x++)
            vga_putc_at((uint8_t)x, (uint8_t)y, ' ');

    /* title row */
    vga_set_color(VGA_BLACK, VGA_LIGHT_GREY);
    int sx = 0;
    vga_write_at_utf8((uint8_t)sx, 0, "CatOS nano  "); sx += 12;
    vga_write_at_utf8((uint8_t)sx, 0, ed_fname); sx += ed_strlen(ed_fname);
    vga_write_at_utf8((uint8_t)sx, 0, "  "); sx += 2;
    char tmp[24];
    itoa(ed_nlines, tmp, 0);
    vga_write_at_utf8((uint8_t)sx, 0, tmp); sx += ed_strlen(tmp);
    vga_write_at_utf8((uint8_t)sx, 0, " lines"); sx += 6;
    if (ed_modified) vga_write_at_utf8((uint8_t)sx, 0, "  [modified]");

    /* content rows (UTF-8 aware, so Chinese file contents display) */
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    for (int r = 0; r < view && r + 1 < last; r++) {
        int ln = ed_top + r;
        if (ln >= ed_nlines) break;
        vga_write_at_utf8(0, (uint8_t)(1 + r), ed_lines[ln]);
    }

    /* bottom bar */
    vga_set_color(VGA_WHITE, VGA_LIGHT_BLUE);
    vga_write_at_utf8(0, (uint8_t)last,
        "^G Help  ^O Save  ^X Exit  ^K Del Line  ^U Clear  F2 IME");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);

    int scr_y = 1 + (ed_cy - ed_top);
    if (scr_y < 1) scr_y = 1;
    if (scr_y > last - 1) scr_y = last - 1;
    vga_set_cursor((uint8_t)ed_cx, (uint8_t)scr_y);
}

static void ed_insert(char c) {
    char *ln = ed_lines[ed_cy];
    int l = ed_strlen(ln);
    int lim = ed_max_cols();
    if (l >= lim) { ed_status_msg("Line is full for this terminal width"); return; }
    for (int i = l; i > ed_cx; i--) ln[i] = ln[i - 1];
    ln[ed_cx] = c;
    ln[l + 1] = 0;
    ed_cx++;
}

/* Insert a whole UTF-8 sequence (from the pinyin IME) as one unit. */
static void ed_insert_utf8(const char *s) {
    while (*s) ed_insert(*s++);
}

static void ed_backspace(void) {
    char *ln = ed_lines[ed_cy];
    if (ed_cx > 0) {
        int l = ed_strlen(ln);
        for (int i = ed_cx - 1; i < l; i++) ln[i] = ln[i + 1];
        ed_cx--;
    } else if (ed_cy > 0) {
        char *prev = ed_lines[ed_cy - 1];
        int pl = ed_strlen(prev);
        char *cur = ed_lines[ed_cy];
        int cl = ed_strlen(cur);
        if (pl + cl > ED_MAX_COLS) cl = ED_MAX_COLS - pl;
        for (int i = 0; i < cl; i++) prev[pl + i] = cur[i];
        prev[pl + cl] = 0;
        for (int i = ed_cy; i < ed_nlines - 1; i++)
            for (int j = 0; j <= ED_MAX_COLS; j++) ed_lines[i][j] = ed_lines[i + 1][j];
        ed_nlines--;
        ed_cy--;
        ed_cx = pl;
    }
}

static void ed_delete(void) {
    char *ln = ed_lines[ed_cy];
    int l = ed_strlen(ln);
    if (ed_cx < l) {
        for (int i = ed_cx; i < l; i++) ln[i] = ln[i + 1];
    } else if (ed_cy < ed_nlines - 1) {
        char *next = ed_lines[ed_cy + 1];
        int nl = ed_strlen(next);
        if (l + nl > ED_MAX_COLS) nl = ED_MAX_COLS - l;
        for (int i = 0; i < nl; i++) ln[l + i] = next[i];
        ln[l + nl] = 0;
        for (int i = ed_cy + 1; i < ed_nlines - 1; i++)
            for (int j = 0; j <= ED_MAX_COLS; j++) ed_lines[i][j] = ed_lines[i + 1][j];
        ed_nlines--;
    }
}

static void ed_enter(void) {
    char *ln = ed_lines[ed_cy];
    int l = ed_strlen(ln);
    if (ed_nlines >= ED_MAX_LINES) { ed_status_msg("Too many lines (max 64)"); return; }
    for (int i = ed_nlines; i > ed_cy + 1; i--)
        for (int j = 0; j <= ED_MAX_COLS; j++) ed_lines[i][j] = ed_lines[i - 1][j];
    int start = ed_cx;
    if (start > l) start = l;
    char *newln = ed_lines[ed_cy + 1];
    int nl = 0;
    for (int i = start; i < l; i++) {
        newln[nl++] = ln[i];
        if (nl > ED_MAX_COLS) break;
    }
    newln[nl] = 0;
    ln[start] = 0;
    ed_nlines++;
    ed_cy++;
    ed_cx = 0;
}

static void ed_del_line(void) {
    for (int i = ed_cy; i < ed_nlines - 1; i++)
        for (int j = 0; j <= ED_MAX_COLS; j++) ed_lines[i][j] = ed_lines[i + 1][j];
    ed_nlines--;
    if (ed_nlines == 0) { ed_lines[0][0] = 0; ed_nlines = 1; }
    if (ed_cy > ed_nlines - 1) ed_cy = ed_nlines - 1;
    if (ed_cx > ed_strlen(ed_lines[ed_cy])) ed_cx = ed_strlen(ed_lines[ed_cy]);
}

static void cmd_edit(const char *arg) {
    if (!*arg) { vga_write("usage: edit FILE\n"); return; }
    /* refuse files exceeding the editor buffer: loading used
     * to silently truncate them and ^O then overwrote the file */
    {
        uint32_t cl = 0, sz = 0;
        if (fs_resolve_file(arg, &cl, &sz) == 0 && sz >= FS_DATA_MAX) {
            vga_write("edit: file too large for the editor;");
            vga_write(" refusing (a save would truncate it)\n");
            return;
        }
    }
    fs_entry_t *f = fs_create_file(arg);
    if (!f) { vga_write("edit: cannot create/open file\n"); return; }

    int fi = 0;
    while (arg[fi] && fi < FS_NAME_MAX - 1) { ed_fname[fi] = arg[fi]; fi++; }
    ed_fname[fi] = 0;

    ed_load(f);
    ed_redraw();

    for (;;) {
        int k = kb_getkey();

        /* v2.9: F2 pinyin IME also works inside the editor */
        {
            char han[5];
            int ir = ime_feed(k, han);
            if (ir == IME_CONSUME) { ed_redraw(); ime_refresh(); continue; }
            if (ir == IME_COMMIT) {
                ed_insert_utf8(han);
                ed_modified = 1;
                ed_redraw(); ime_refresh();
                continue;
            }
        }

        if (k == 0x18) {                    /* ^X exit */
            int do_exit = 1;
            if (ed_modified) {
                ed_status_msg("Save?  ^O=yes  ^Y=no  any other=cancel");
                for (;;) {
                    k = kb_getkey();
                    if (k == 0x0F) { ed_save(f); break; }   /* ^O */
                    if (k == 0x19) { break; }                /* ^Y */
                    ed_redraw();
                    do_exit = 0;
                    break;
                }
                if (!do_exit) continue;
            }
            vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
            ime_off();
            vga_clear();
            vga_write("edit: saved ");
            print_int(fs_file_size(f));
            vga_write(" bytes.\n");
            return;
        }
        if (k == 0x0F || k == 0x13) {       /* ^O / ^S save */
            if (ed_save(f) != 0)
                ed_status_msg("SAVE FAILED: exceeds editor buffer limit");
            else
                ed_status_msg("Saved.");
            continue;
        }
        if (k == 0x07) {                    /* ^G help */
            ed_status_msg("^O/^S save  ^X exit  ^K del line  ^U clear  arrows move  Del delete");
            continue;
        }
        if (k == 0x0B) { ed_del_line(); ed_modified = 1; ed_redraw(); continue; }  /* ^K */
        if (k == 0x15) {                    /* ^U clear all */
            ed_nlines = 1; ed_lines[0][0] = 0;
            ed_cy = 0; ed_cx = 0; ed_top = 0; ed_modified = 1; ed_redraw(); continue;
        }
        if (k == KB_UP)          { if (ed_cy > 0) ed_cy--; }
        else if (k == KB_DOWN)   { if (ed_cy < ed_nlines - 1) ed_cy++; }
        else if (k == KB_LEFT)   { if (ed_cx > 0) ed_cx--; }
        else if (k == KB_RIGHT)  { int l = ed_strlen(ed_lines[ed_cy]); if (ed_cx < l) ed_cx++; }
        else if (k == KB_HOME)   { ed_cx = 0; }
        else if (k == KB_END)    { ed_cx = ed_strlen(ed_lines[ed_cy]); }
        else if (k == KB_PGUP)   { ed_cy -= ed_view_rows(); if (ed_cy < 0) ed_cy = 0; }
        else if (k == KB_PGDN)   { ed_cy += ed_view_rows(); if (ed_cy > ed_nlines - 1) ed_cy = ed_nlines - 1; }
        else if (k == '\b')      { ed_backspace(); ed_modified = 1; }
        else if (k == KB_DEL)    { ed_delete(); ed_modified = 1; }
        else if (k == '\n')      { ed_enter(); ed_modified = 1; }
        else if (k >= 32 && k < 256) { ed_insert((char)k); ed_modified = 1; }

        int ll = ed_strlen(ed_lines[ed_cy]);
        if (ed_cx > ll) ed_cx = ll;
        if (ed_cx > ed_max_cols()) ed_cx = ed_max_cols();
        if (ed_cy < ed_top) ed_top = ed_cy;
        if (ed_cy >= ed_top + ed_view_rows())
            ed_top = ed_cy - ed_view_rows() + 1;
        ed_redraw();
        ime_refresh();
    }
}

/* ---------- session user & capability commands (v2.13) --------------------
 * The session starts as root (the initial user).  `login`/`su` switch it;
 * `useradd`/`userdel`/`passwd`/`chcaps` edit the persistent /etc/passwd;
 * `chmod`/`chown` edit the persistent /etc/acl.  Everything here runs in
 * ring0 and talks to user.c directly -- no new syscall slots. */
static int atoi_(const char *s) {
    int v = 0, neg = 0;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}
static uint32_t hex32_(const char *s) {
    uint32_t v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    for (;;) {
        int d;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
        else break;
        v = v * 16u + (uint32_t)d;
        s++;
    }
    return v;
}
static uint32_t octal32_(const char *s) {
    uint32_t v = 0;
    while (*s >= '0' && *s <= '7') { v = v * 8u + (uint32_t)(*s++ - '0'); }
    return v;
}
/* resolve a possibly-relative path against the cwd (fs_pwd gives cwd) */
static void abs_path_(const char *in, char *out, int outlen) {
    if (!in || !*in || in[0] == '/') {
        int i = 0;
        for (; in && in[i] && i < outlen - 1; i++) out[i] = in[i];
        out[i] = 0;
        return;
    }
    char pwd[FS_PATH_MAX];
    fs_pwd(pwd, sizeof(pwd));
    int o = 0;
    for (int i = 0; pwd[i] && o < outlen - 1; i++) out[o++] = pwd[i];
    if (o > 0 && out[o - 1] != '/' && o < outlen - 1) out[o++] = '/';
    for (int i = 0; in[i] && o < outlen - 1; i++) out[o++] = in[i];
    out[o] = 0;
}

static int      g_uid  = UID_ROOT;
static int      g_gid  = 0;
static uint32_t g_caps = CAP_ALL;

static void session_apply(void) {
    user_rec_t *u = user_by_uid(g_uid);
    if (u) { g_gid = u->gid; g_caps = u->caps; }
}

static void print_caps(uint32_t caps) {
    if (caps == CAP_ALL) { vga_write("all"); return; }
    static const struct { uint32_t bit; const char *nm; } T[] = {
        { CAP_NET, "net" },      { CAP_KILL, "kill" },
        { CAP_FSADMIN, "fsadmin" }, { CAP_AUDIO, "audio" },
        { CAP_USERADM, "useradm" }, { CAP_DEVICE, "device" },
        { CAP_SYS, "sys" },
        { CAP_FORMAT, "format" }, { CAP_FSCK, "fsck" },
        { CAP_RAWIO, "rawio" },   { CAP_HWINFO, "hwinfo" },
        { CAP_REBOOT, "reboot" }, { CAP_DIAG, "diag" },
    };
    int first = 1;
    for (unsigned i = 0; i < sizeof(T) / sizeof(T[0]); i++)
        if (caps & T[i].bit) {
            if (!first) vga_write(",");
            vga_write(T[i].nm);
            first = 0;
        }
    if (first) vga_write("none");
}

static int session_is_admin(void) {
    return g_uid == UID_ROOT || (g_caps & CAP_USERADM);
}
static int session_is_fsadmin(void) {
    return g_uid == UID_ROOT || (g_caps & CAP_FSADMIN);
}

static void cmd_whoami(void) {
    user_rec_t *u = user_by_uid(g_uid);
    vga_write(u ? u->name : "?");
    vga_write("\n");
}

static void cmd_id(const char *name) {
    user_rec_t *u = (name && *name) ? user_by_name(name) : user_by_uid(g_uid);
    if (!u) { vga_write("id: no such user\n"); return; }
    vga_write("uid=");    print_int(u->uid);
    vga_write(" gid=");   print_int(u->gid);
    vga_write(" name=");  vga_write(u->name);
    vga_write(" home=");  vga_write(u->home);
    vga_write("\ncaps=0x"); print_hex(u->caps);
    vga_write(" ("); print_caps(u->caps); vga_write(")\n");
    vga_write("pw="); vga_write(user_pw_is_set(u) ? "set" : "none");
    vga_write(" shell="); vga_write(u->shell); vga_write("\n");
}

static void print_hex8(uint32_t v) {
    char b[9];
    for (int i = 7; i >= 0; i--) { b[i] = "0123456789ABCDEF"[v & 0xF]; v >>= 4; }
    b[8] = 0;
    vga_write(b);
}

static void cmd_lsuser(void) {
    vga_write("UID  GID  PW   CAPS      NAME\n");
    for (int i = 0; i < MAX_USERS; i++) {
        user_rec_t *u = user_at(i);
        if (!u || !u->used) continue;
        print_pad(u->uid, 4); vga_write(" ");
        print_pad(u->gid, 4); vga_write(" ");
        vga_write(user_pw_is_set(u) ? "yes  " : "no   ");
        print_hex8(u->caps);
        vga_write("  ");
        vga_write(u->name);
        vga_write("\n");
    }
}

static void cmd_caps_legend(void) {
    vga_write("capability bits (per-user, /etc/passwd):\n");
    vga_write("  0x0001 net      network: http/dns/udp/ping/ip\n");
    vga_write("  0x0002 kill     kill tasks not owned by self\n");
    vga_write("  0x0004 fsadmin  format / fsck\n");
    vga_write("  0x0008 audio    SB16 PCM playback\n");
    vga_write("  0x0010 useradm  useradd/userdel/passwd/chcaps/su\n");
    vga_write("  0x0020 device   raw device + low-level hw info\n");
    vga_write("  0x0040 sys      reserved\n");
    vga_write("  fine-grained (v2.25c splits of fsadmin/device):\n");
    vga_write("  0x0080 format   0x0100 fsck    0x0200 rawio\n");
    vga_write("  0x0400 hwinfo   0x0800 reboot  0x1000 diag\n");
    vga_write("session: uid="); print_int(g_uid);
    vga_write(" caps=0x"); print_hex(g_caps);
    vga_write(" ("); print_caps(g_caps); vga_write(")\n");
}

/* login USER [PW]  /  su [USER [PW]] */
static void cmd_login(char *arg, int as_su) {
    char *argv[3];
    int argc = split_words(arg, argv, 3);
    const char *name = argc >= 1 ? argv[0] : "root";
    const char *pw   = argc >= 2 ? argv[1] : 0;
    user_rec_t *u = user_by_name(name);
    if (!u) { vga_write("login: no such user\n"); return; }
    if (u->uid == g_uid) { vga_write("already session user\n"); return; }
    int need_pw = user_pw_is_set(u);
    if (as_su && g_uid == UID_ROOT) need_pw = 0;      /* root may su freely */
    /* T52: when no password was given on the command line, ask for it
     * interactively and echo '*' per keystroke instead of the characters. */
    char typed[64];
    if (need_pw && !pw) {
        vga_write(tr(L_PASSWD_PROMPT));
        kb_readline_masked(typed, sizeof(typed));
        pw = typed;
    }
    if (need_pw && user_check_pw(u, pw) != 0) {
        vga_write("login: incorrect password\n");
        return;
    }
    g_uid = u->uid;
    session_apply();
    vga_write("logged in as "); vga_write(u->name);
    vga_write(" (uid "); print_int(g_uid);
    vga_write(")\n");
}

/* useradd NAME [UID [GID [CAPS_HEX]]]
 * With no CAPS_HEX argument the capability set is built interactively: one
 * yes/no prompt per capability bit (T52).  `useradd NAME UID GID CAPS` keeps
 * working for scripts and for the regression suite. */
static void cmd_useradd(char *arg) {
    if (!session_is_admin()) { vga_write("useradd: permission denied\n"); return; }
    char *argv[5];
    int argc = split_words(arg, argv, 5);
    if (argc < 1) {
        vga_write("usage: useradd NAME [UID [GID [CAPS_HEX]]]\n");
        vga_write("       with no CAPS_HEX the capability wizard starts\n");
        vga_write("       e.g. useradd guest 101 101 0   (no capabilities)\n");
        return;
    }
    const char *name = argv[0];
    int uid  = argc >= 2 ? atoi_(argv[1]) : 101;
    int gid  = argc >= 3 ? atoi_(argv[2]) : uid;
    uint32_t caps;
    if (argc >= 4) {
        caps = hex32_(argv[3]);
    } else {
        /* T52 wizard, extended v2.25: presets first, then per-bit
         * questions including the fine-grained successors. */
        static const struct { uint32_t bit; const char *q; } W[] = {
            { CAP_NET,     "network (http/dns/udp/ping)" },
            { CAP_KILL,    "kill other users' tasks" },
            { CAP_FSADMIN, "filesystem admin (format+fsck, broad)" },
            { CAP_FORMAT,  "format disks (fine)" },
            { CAP_FSCK,    "fsck mirror repair (fine)" },
            { CAP_AUDIO,   "audio playback (SB16/ES1371)" },
            { CAP_USERADM, "user admin (useradd/passwd/su)" },
            { CAP_DEVICE,  "raw device access (broad)" },
            { CAP_RAWIO,   "raw device I/O (fine)" },
            { CAP_HWINFO,  "hardware info read (fine)" },
            { CAP_SYS,     "system (broad: reboot+introspection)" },
            { CAP_REBOOT,  "reboot (fine)" },
            { CAP_DIAG,    "kernel introspection/faultinfo (fine)" },
        };
        caps = 0;
        vga_write("Capabilities for '"); vga_write(name);
        vga_write("'\n  preset? admin/operator/developer/viewer/guest or ENTER for wizard: ");
        char pans[16];
        kb_readline(pans, sizeof(pans));
        if (pans[0] && user_preset_caps(pans, &caps) == 0) {
            vga_write("     preset '"); vga_write(pans);
            vga_write("' -> caps=0x"); print_hex(caps); vga_write("\n");
        } else {
        vga_write("(y/N per item)\n");
        char ans[8];
        for (int i = 0; i < (int)(sizeof(W) / sizeof(W[0])); i++) {
            char num[4]; num[0] = (char)('1' + i); num[1] = 0;
            vga_write("  "); vga_write(num); vga_write(". allow ");
            vga_write(W[i].q); vga_write("? [y/N] ");
            kb_readline(ans, sizeof(ans));
            if (ans[0] == 'y' || ans[0] == 'Y') {
                caps |= W[i].bit;
                vga_write("     -> granted\n");
            } else {
                vga_write("     -> denied\n");
            }
        }
        vga_write("       caps=0x"); print_hex(caps); vga_write("\n");
        }
    }
    char home[48];
    int o = 0;
    for (const char *q = "/home/"; *q && o < 47; q++) home[o++] = *q;
    for (const char *q = name; *q && o < 47; q++) home[o++] = *q;
    home[o] = 0;
    int r = user_add(name, uid, gid, home, "/bin/shell.ce", caps);
    if (r == -5) { vga_write("useradd: name or uid already exists\n"); return; }
    if (r != 0)  { vga_write("useradd: failed (table full?)\n"); return; }
    if (user_save() != 0) vga_write("useradd: WARNING: could not persist /etc/passwd\n");
    else vga_write("user added (persisted to /etc/passwd)\n");
}

static void cmd_userdel(char *arg) {
    if (!session_is_admin()) { vga_write("userdel: permission denied\n"); return; }
    char *argv[2];
    if (split_words(arg, argv, 2) < 1) { vga_write("usage: userdel NAME\n"); return; }
    if (!strcmp_(argv[0], "root")) { vga_write("userdel: root is undeletable\n"); return; }
    if (atoi_(argv[0]) == 0 && argv[0][0] >= '0' && argv[0][0] <= '9') {
        /* numeric arg: refuse deleting root by number too */
    }
    int r = user_del(argv[0]);
    if (r == -1) { vga_write("userdel: no such user\n"); return; }
    if (user_save() != 0) vga_write("userdel: WARNING: could not persist\n");
    else vga_write("user deleted\n");
}

static void cmd_passwd(char *arg) {
    char *argv[3];
    int argc = split_words(arg, argv, 3);
    const char *name = argc >= 1 ? argv[0] : (user_by_uid(g_uid) ? user_by_uid(g_uid)->name : 0);
    if (!name) { vga_write("usage: passwd [NAME] [NEWPW]\n"); return; }
    user_rec_t *u = user_by_name(name);
    if (!u) { vga_write("passwd: no such user\n"); return; }
    if (u->uid != g_uid && !session_is_admin()) {
        vga_write("passwd: permission denied\n");
        return;
    }
    const char *pw;
    char p1[64], p2[64];
    if (argc >= 2) {
        pw = argv[1];                        /* non-interactive form */
    } else {
        /* T52: interactive, no echo -- '*' per keystroke, retype check */
        vga_write("New password for "); vga_write(u->name); vga_write(": ");
        kb_readline_masked(p1, sizeof(p1));
        vga_write("Retype new password: ");
        kb_readline_masked(p2, sizeof(p2));
        if (!p1[0]) { vga_write("passwd: empty password rejected\n"); return; }
        if (strcmp_(p1, p2) != 0) { vga_write(tr(L_PASSWD_MISMATCH)); return; }
        pw = p1;
    }
    user_set_pw(name, pw);
    if (user_save() != 0) vga_write("passwd: WARNING: could not persist\n");
    else vga_write("password set\n");
}

static void cmd_chcaps(char *arg) {
    if (!session_is_admin()) { vga_write("chcaps: permission denied\n"); return; }
    char *argv[3];
    if (split_words(arg, argv, 3) < 2) { vga_write("usage: chcaps CAPS_HEX NAME\n"); return; }
    if (user_set_caps(argv[1], hex32_(argv[0])) != 0) {
        vga_write("chcaps: no such user\n");
        return;
    }
    if (user_save() != 0) vga_write("chcaps: WARNING: could not persist\n");
    else vga_write("capabilities updated\n");
    user_rec_t *tu = user_by_name(argv[1]);
    if (tu && g_uid == tu->uid) session_apply();
}

/* chmod OCTAL PATH   (owner or root) */
static void cmd_chmod(char *arg) {
    char *argv[3];
    if (split_words(arg, argv, 3) < 2) { vga_write("usage: chmod OCTAL PATH\n"); return; }
    int mode = (int)octal32_(argv[0]);
    char path[FS_PATH_MAX];
    abs_path_(argv[1], path, sizeof(path));
    /* #98: acl_lookup() leaves its out-params untouched on a miss.  All
     * three MUST be initialised here -- the old code passed uninitialised
     * fgid/fmode straight into acl_set() and persisted stack garbage. */
    int fuid = UID_ROOT, fgid = 0, fmode = 0644;
    if (acl_lookup(path, &fuid, &fgid, &fmode) != 0) {
        /* default entry: root-owned 0644 -- only root may chmod it */
        fuid = UID_ROOT; fgid = 0; fmode = 0644;
    }
    if (g_uid != UID_ROOT && g_uid != fuid) {
        vga_write("chmod: not the owner\n");
        return;
    }
    if (acl_set(path, fuid, fgid, mode) != 0) {
        vga_write("chmod: acl table full\n");
        return;
    }
    if (acl_save() != 0) vga_write("chmod: WARNING: could not persist\n");
    else vga_write("mode updated\n");
}

/* chown UID[:GID] PATH   (root or CAP_FSADMIN) */
static void cmd_chown(char *arg) {
    if (!session_is_fsadmin()) { vga_write("chown: permission denied\n"); return; }
    char *argv[3];
    if (split_words(arg, argv, 3) < 2) { vga_write("usage: chown UID[:GID] PATH\n"); return; }
    int uid = 0, gid = -1;
    const char *spec = argv[0];
    int i = 0;
    for (; spec[i] && spec[i] != ':'; i++) ;
    if (spec[i] == ':') {
        char nb[12]; int j = 0;
        for (int k = 0; k < i && j < 11; k++) nb[j++] = spec[k];
        nb[j] = 0; uid = atoi_(nb);
        j = 0;
        for (int k = i + 1; spec[k] && j < 11; k++) nb[j++] = spec[k];
        nb[j] = 0;
        gid = spec[i + 1] ? atoi_(nb) : -1;
    } else uid = atoi_(spec);
    char path[FS_PATH_MAX];
    abs_path_(argv[1], path, sizeof(path));
    int fuid, fgid, fmode;
    if (acl_lookup(path, &fuid, &fgid, &fmode) != 0) { fuid = UID_ROOT; fgid = 0; fmode = 0644; }
    if (gid < 0) gid = fgid;
    if (acl_set(path, uid, gid, fmode) != 0) { vga_write("chown: acl table full\n"); return; }
    if (acl_save() != 0) vga_write("chown: WARNING: could not persist\n");
    else vga_write("owner updated\n");
}

/* ---------- file utilities: cat / rm / cp / mv (v2.14) ------------------
 * Imported from the retired "ot" (Ocean's Tools) user-mode tool REPL:
 * every one of these existed only because the kernel shell lacked them.
 * The kernel side is the right home (consistent permissions, no user-space
 * re-implementation of fs internals).  ot keeps its unique value:
 * csc scripting, fmgr, hexedit, grep, vcs, alias, banner, cecheck. */

#define CAT_MAX_PRINT 65536u      /* cat: stop printing after 64 KiB */

static void cmd_cat(const char *arg) {
    if (!*arg) { vga_write("usage: cat FILE\n"); return; }
    uint32_t c, s;
    if (fs_resolve_file(arg, &c, &s) != 0) { vga_write("cat: no such file\n"); return; }
    if (fs_perm_check(arg, g_uid, g_gid, PERM_READ) != 0) {
        vga_write("cat: permission denied\n"); return;
    }
    static char buf[512];         /* ring0 single shell: no reentry */
    uint32_t off = 0;
    while (off < s) {
        uint32_t want = s - off;
        if (want > sizeof(buf) - 1) want = sizeof(buf) - 1;
        int r = fs_read_at(arg, buf, (int)off, (int)want);
        if (r <= 0) { vga_write("cat: read error\n"); return; }
        buf[r] = 0;
        vga_write(buf);
        off += (uint32_t)r;
        if (off >= CAT_MAX_PRINT && off < s) {
            vga_write("\n[cat: output truncated at 64 KiB]\n");
            return;
        }
    }
    if (off) vga_write("\n");
}

/* v2.24 recursive helpers (rm -r / cp -r).  Depth-capped: every frame
 * keeps two FS_PATH_MAX buffers, so the cap also bounds shell stack use.
 * FAT32 has no symlinks, so no loop risk beyond pathological nesting. */
#define FSREC_DEPTH_MAX 6

static void rec_join(char *out, int cap, const char *dir, const char *leaf) {
    int o = 0;
    for (; *dir && o < cap - 1; dir++) out[o++] = *dir;
    if (o > 0 && out[o - 1] != '/' && o < cap - 1) out[o++] = '/';
    for (; *leaf && o < cap - 1; leaf++) out[o++] = *leaf;
    out[o] = 0;
}

struct cp_ud { const char *dst; int depth; };

static int copy_rec(const char *src, const char *dst, int depth);

/* src prefix for the current fs_foreach walk -- the shell is the only
 * ring0 thread, so a module-static here is race-free (same pattern the
 * job/log code uses). */
static const char *cp_src_prefix;

static void cp_cb(int is_dir, const char *name, uint32_t size, void *ud) {
    (void)size;
    struct cp_ud *u = (struct cp_ud *)ud;
    if (!strcmp_(name, ".") || !strcmp_(name, "..")) return;
    char full_d[FS_PATH_MAX];
    rec_join(full_d, sizeof(full_d), u->dst, name);
    char full_s[FS_PATH_MAX];
    rec_join(full_s, sizeof(full_s), cp_src_prefix, name);
    if (is_dir) copy_rec(full_s, full_d, u->depth + 1);
    else        fs_copy_file(full_s, full_d);
}

static int copy_rec(const char *src, const char *dst, int depth) {
    if (depth > FSREC_DEPTH_MAX) return -5;
    if (fs_is_dir(src)) {
        char parent[FS_PATH_MAX];
        if (perm_write_on_parent(dst, parent, sizeof(parent)) == -13)
            return -13;
        int r = fs_mkdir(dst);
        if (r != FS_OK && r != FS_ERR_EXIST) return -2;
        acl_set(dst, g_uid, g_gid, 0755);
        const char *saved = cp_src_prefix;
        cp_src_prefix = src;
        struct cp_ud u = { dst, depth };
        fs_foreach(src, cp_cb, &u);
        cp_src_prefix = saved;
        return 0;
    }
    return fs_copy_file(src, dst);
}

static int rm_rec(const char *path, int depth);

struct rm_ud { const char *base; int depth; };
static void rm_cb(int is_dir, const char *name, uint32_t size, void *ud) {
    (void)size;
    struct rm_ud *u = (struct rm_ud *)ud;
    if (!strcmp_(name, ".") || !strcmp_(name, "..")) return;
    char full[FS_PATH_MAX];
    rec_join(full, sizeof(full), u->base, name);
    if (is_dir) rm_rec(full, u->depth + 1);
    else {
        if (fs_perm_check(full, g_uid, g_gid, PERM_WRITE) == 0) {
            if (fs_delete(full) == 0) acl_unset(full);
        }
    }
}

static int rm_rec(const char *path, int depth) {
    if (depth > FSREC_DEPTH_MAX) return -5;
    if (!fs_is_dir(path)) return fs_delete(path) == 0 ? 0 : -1;
    if (fs_perm_check(path, g_uid, g_gid, PERM_WRITE) != 0) return -13;
    struct rm_ud u = { path, depth };
    fs_foreach(path, rm_cb, &u);
    int r = fs_rmdir(path);
    if (r == 0) acl_unset(path);
    return r;
}

static void cmd_rm(const char *arg) {
    int rec = 0;
    while (*arg == ' ') arg++;
    if (arg[0] == '-' && (arg[1] == 'r' || arg[1] == 'R')) {
        rec = 1;
        arg += 2;
        if (*arg == 'f') arg++;
        while (*arg == ' ') arg++;
    }
    if (!*arg) { vga_write("usage: rm [-r] FILE|DIR\n"); return; }
    if (fs_is_dir(arg)) {
        if (!rec) { vga_write("rm: is a directory (use rm -r)\n"); return; }
        char abs[FS_PATH_MAX];
        fs_canon_path(arg, abs, sizeof(abs));
        if (!strcmp_(abs, "/")) { vga_write("rm: refusing to remove /\n"); return; }
        int r = rm_rec(arg, 0);
        if (r == 0) return;
        if (r == -13) vga_write("rm: permission denied\n");
        else if (r == -5) vga_write("rm: nesting too deep\n");
        else vga_write("rm: directory remove failed (entries left behind?)\n");
        return;
    }
    if (fs_perm_check(arg, g_uid, g_gid, PERM_WRITE) != 0) {
        vga_write("rm: permission denied\n"); return;
    }
    int r = fs_delete(arg);
    if (r == 0) { acl_unset(arg); return; }
    vga_write("rm: no such file\n");
}

/* shared copy core for cp / mv: read src in <=16 KiB chunks, append to a
 * freshly truncated dst.  Returns 0 ok, negative error code. */
static int fs_copy_file(const char *src, const char *dst) {
    uint32_t c, s;
    if (fs_resolve_file(src, &c, &s) != 0) return -1;
    if (fs_perm_check(src, g_uid, g_gid, PERM_READ) != 0) return -13;
    if (fs_perm_check(dst, g_uid, g_gid, PERM_WRITE) != 0) return -13;
    static char chunk[16384];     /* ring0 single shell: no reentry */
    if (fs_write_raw_mode(dst, "", 0, 0) != 0) return -2;   /* create/truncate */
    acl_set(dst, g_uid, g_gid, 0755);   /* creator owns it; 0755 matches
                                     * the documented "no entry" default so a
                                     * copied .ce stays runnable (v2.24) */
    uint32_t off = 0;
    while (off < s) {
        uint32_t want = s - off;
        if (want > sizeof(chunk)) want = sizeof(chunk);
        int r = fs_read_at(src, chunk, (int)off, (int)want);
        if (r <= 0) return -3;
        if (fs_write_raw_mode(dst, chunk, (uint32_t)r, 1) != 0) return -4;
        off += (uint32_t)r;
    }
    return 0;
}

static void cmd_cp(char *arg) {
    int rec = 0;
    while (*arg == ' ') arg++;
    if (arg[0] == '-' && (arg[1] == 'r' || arg[1] == 'R')) {
        rec = 1;
        arg += 2;
        while (*arg == ' ') arg++;
    }
    char *src = arg;
    while (*arg && *arg != ' ') arg++;
    if (!*arg) { vga_write("usage: cp SRC DST\n"); return; }
    *arg++ = 0;
    while (*arg == ' ') arg++;
    char *dst = arg;
    while (*arg && *arg != ' ') arg++;
    *arg = 0;
    if (!*src || !*dst) { vga_write("usage: cp [-r] SRC DST\n"); return; }
    if (rec && fs_is_dir(src)) {
        char abs[FS_PATH_MAX];
        fs_canon_path(src, abs, sizeof(abs));
        if (!strcmp_(abs, "/")) { vga_write("cp: refusing to copy /\n"); return; }
        int r = copy_rec(src, dst, 0);
        if (r == 0) return;
        if (r == -13) vga_write("cp: permission denied\n");
        else if (r == -2) vga_write("cp: cannot create target directory\n");
        else if (r == -5) vga_write("cp: nesting too deep\n");
        else              vga_write("cp: copy failed mid-way\n");
        return;
    }
    int r = fs_copy_file(src, dst);
    if (r == 0) return;
    if (r == -13) vga_write("cp: permission denied\n");
    else if (r == -1) vga_write("cp: no such file\n");
    else if (r == -2) vga_write("cp: cannot create target\n");
    else              vga_write("cp: copy failed mid-way\n");
}

static void cmd_mv(char *arg) {
    while (*arg == ' ') arg++;
    char *src = arg;
    while (*arg && *arg != ' ') arg++;
    if (!*arg) { vga_write("usage: mv SRC DST\n"); return; }
    *arg++ = 0;
    while (*arg == ' ') arg++;
    char *dst = arg;
    while (*arg && *arg != ' ') arg++;
    *arg = 0;
    if (!*src || !*dst) { vga_write("usage: mv SRC DST\n"); return; }
    int r = fs_copy_file(src, dst);
    if (r == 0) {
        /* copy complete -> remove the source (write perm on it) */
        if (fs_perm_check(src, g_uid, g_gid, PERM_WRITE) != 0) {
            vga_write("mv: copied but cannot delete source (permission)\n");
            return;
        }
        if (fs_delete(src) != 0) {
            vga_write("mv: copied but could not delete source\n");
            return;
        }
        acl_unset(src);
        return;
    }
    if (r == -13) vga_write("mv: permission denied\n");
    else if (r == -1) vga_write("mv: no such file\n");
    else if (r == -2) vga_write("mv: cannot create target\n");
    else              vga_write("mv: move failed mid-way\n");
}

/* ---------- v2.23 environment variables (session table + /etc/env) -------
 * Windows/Linux both expose process/session environment variables; CatOS's
 * shell is ring0 and single-session, so a flat table is honest here.
 *   export KEY=value   set (empty value deletes, like POSIX unset semantics)
 *   export KEY=        delete
 *   export / env       list
 *   unset KEY          delete
 *   $KEY / ${KEY}      expanded in typed lines and autostart scripts
 *   PATH               drives bare-NAME program lookup (default /bin)
 * Persistence: /etc/env, one KEY=VALUE per line, saved on every change
 * (same on-demand style as /etc/lang).  CE apps do NOT inherit env yet --
 * passing an environment block needs an append-only ABI slot, tracked in
 * PROGRESS as the next step. */
#define ENV_MAX     16
#define ENV_KEY_MAX 16
#define ENV_VAL_MAX 64
typedef struct { char key[ENV_KEY_MAX]; char val[ENV_VAL_MAX]; } env_rec_t;
static env_rec_t g_env[ENV_MAX];
static int g_env_used[ENV_MAX];

static int env_key_ok(const char *k) {
    if (!k || !*k) return 0;
    for (int i = 0; k[i]; i++) {
        char c = k[i];
        int ok = (c == '_') ||
                 (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                 (i > 0 && c >= '0' && c <= '9');
        if (!ok) return 0;
    }
    return slen_(k) < ENV_KEY_MAX;
}

static const char *env_get_(const char *key) {
    if (!env_key_ok(key)) return 0;
    for (int i = 0; i < ENV_MAX; i++)
        if (g_env_used[i] && !strcmp_(g_env[i].key, key)) return g_env[i].val;
    return 0;
}

/* returns 0 set, 1 deleted, -1 table full / bad key */
static int env_set_(const char *key, const char *val) {
    if (!env_key_ok(key)) return -1;
    int slot = -1;
    for (int i = 0; i < ENV_MAX; i++) {
        if (g_env_used[i] && !strcmp_(g_env[i].key, key)) { slot = i; break; }
        if (slot < 0 && !g_env_used[i]) slot = i;
    }
    if (slot < 0) return -1;
    if (val && *val) {
        if (slen_(val) >= ENV_VAL_MAX || slen_(key) >= ENV_KEY_MAX) return -1;
        int i = 0;
        for (; key[i]; i++) g_env[slot].key[i] = key[i];
        g_env[slot].key[i] = 0;
        for (i = 0; val[i]; i++) g_env[slot].val[i] = val[i];
        g_env[slot].val[i] = 0;
        g_env_used[slot] = 1;
        return 0;
    }
    g_env_used[slot] = 0;               /* empty value deletes */
    return 1;
}

static void env_list(void) {
    for (int i = 0; i < ENV_MAX; i++) {
        if (!g_env_used[i]) continue;
        vga_write(g_env[i].key);
        vga_write("=");
        vga_write(g_env[i].val);
        vga_write("\n");
    }
}

int env_save(void) {
    static char buf[2048];
    int o = 0;
    for (int i = 0; i < ENV_MAX; i++) {
        if (!g_env_used[i]) continue;
        for (int j = 0; g_env[i].key[j]; j++) buf[o++] = g_env[i].key[j];
        buf[o++] = '=';
        for (int j = 0; g_env[i].val[j]; j++) buf[o++] = g_env[i].val[j];
        buf[o++] = '\n';
    }
    if (o == 0) return 0;               /* empty table: nothing to persist */
    return fs_write_text("/etc/env", buf, (uint32_t)o) == 0 ? 0 : -1;
}

void env_init(void) {
    for (int i = 0; i < ENV_MAX; i++) g_env_used[i] = 0;
    if (!fs_fs_ready()) return;
    static char buf[2048];
    int n = fs_read_text("/etc/env", buf, (int)sizeof(buf) - 1);
    int fresh = (n <= 0);
    if (n > 0) {
        buf[n] = 0;
        char *p = buf;
        while (*p) {
            char *line = p;
            while (*p && *p != '\n') p++;
            if (*p) *p++ = 0;
            char *eq = line;
            while (*eq && *eq != '=') eq++;
            if (!*eq) continue;
            *eq = 0;
            env_set_(line, eq + 1);     /* bad keys silently skipped */
        }
    }
    if (!env_get_("PATH")) env_set_("PATH", "/bin");
    if (fresh) env_save();              /* seed /etc/env with PATH=/bin */
}

/* expand $KEY / ${KEY} in place.  Unknown names expand to empty (sh
 * semantics).  "$$" is a literal '$'. */
static void env_expand(char *line, int cap) {
    char out[256];
    int o = 0;
    for (int i = 0; line[i] && o < (int)sizeof(out) - 1; ) {
        if (line[i] != '$') { out[o++] = line[i++]; continue; }
        if (line[i + 1] == '$') { out[o++] = '$'; i += 2; continue; }
        int brace = 0;
        int j = i + 1;                      /* skip the '$' itself */
        if (line[j] == '{') { brace = 1; j++; }
        char key[ENV_KEY_MAX];
        int k = 0;
        for (; line[j] && k < ENV_KEY_MAX - 1; j++) {
            char c = line[j];
            int ok = (c == '_') ||
                     (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                     (k > 0 && c >= '0' && c <= '9');
            if (!ok) break;
            key[k++] = c;
        }
        key[k] = 0;
        if (brace && line[j] == '}') j++;
        i = j;
        if (!k) { if (o < (int)sizeof(out) - 1) out[o++] = '$'; continue; }
        const char *v = env_get_(key);
        if (v) for (; *v && o < (int)sizeof(out) - 1; v++) out[o++] = *v;
    }
    out[o] = 0;
    int j = 0;
    for (; out[j] && j < cap - 1; j++) line[j] = out[j];
    line[j] = 0;
}

static void cmd_export(char *arg) {
    if (!arg || !*arg) { env_list(); return; }
    char *eq = arg;
    while (*eq && *eq != '=') eq++;
    if (!*eq) { vga_write("usage: export KEY=VALUE | export KEY= | export | env\n"); return; }
    *eq = 0;
    int r = env_set_(arg, eq + 1);
    if (r == -1) { vga_write("export: bad key or table full or value too long\n"); return; }
    if (r == 0) vga_write("export: "); else vga_write("unset: ");
    vga_write(arg);
    if (r == 0) { vga_write("="); vga_write(eq + 1); }
    vga_write("\n");
    if (env_save() != 0)
        vga_write(tr(L_ENV_NOSAVE));
}

static void cmd_unset(char *arg) {
    if (!arg || !*arg) { vga_write("usage: unset KEY\n"); return; }
    char key[ENV_KEY_MAX];
    int i = 0;
    for (; arg[i] && i < ENV_KEY_MAX - 1; i++) key[i] = arg[i];
    key[i] = 0;
    if (env_set_(key, "") != 1) { vga_write("unset: no such variable\n"); return; }
    vga_write("unset: ");
    vga_write(key);
    vga_write("\n");
    if (env_save() != 0)
        vga_write(tr(L_ENV_NOSAVE));
}

/* ---------- v2.25 glyph scale (`size`, persisted in /etc/size) ---------- */
static const char *size_name(int s) {
    if (s == 0) return "compact  (8x8 cell, 100x75)";
    if (s == 2) return "large    (16x32 cell, 50x18)";
    return "normal   (8x16 cell, 100x37)";
}

static void size_apply(int s) {
    if (con_set_cell_size(s) == 0) {
        char b[2] = { (char)('0' + s), 0 };
        fs_write_text("/etc/size", b, 1);
    }
}

/* Re-apply the persisted scale when entering vbe24 (boot default 1). */
static void size_restore(void) {
    char b[8];
    int n = fs_read_text("/etc/size", b, (int)sizeof(b) - 1);
    if (n < 1) return;
    b[n] = 0;
    if (b[0] >= '0' && b[0] <= '2') con_set_cell_size(b[0] - '0');
}

static void cmd_size(const char *arg) {
    if (con_get_mode() != CON_MODE_VBE24) {
        vga_write("size: only supported in vbe24 mode (run `gfx24` first)\n");
        return;
    }
    int s = con_info(CON_INFO_CELL_SCALE);
    if (!*arg) {
        vga_write("size: current "); vga_write(size_name(s));
        vga_write("\nusage: size compact|normal|large  (rows x cols change with\n");
        vga_write("       the cell size; layout is re-flowed, choice persists)\n");
        return;
    }
    if (!strcmp_(arg, "compact") || !strcmp_(arg, "small") ||
        arg[0] == '0')      size_apply(0);
    else if (!strcmp_(arg, "normal") || arg[0] == '1') size_apply(1);
    else if (!strcmp_(arg, "large") || !strcmp_(arg, "big") ||
        arg[0] == '2')      size_apply(2);
    else { vga_write("size: compact|normal|large\n"); return; }
    vga_write("size: now "); vga_write(size_name(con_info(CON_INFO_CELL_SCALE)));
    vga_write(" -- grid "); print_int(con_cols());
    vga_write(" x "); print_int(con_rows()); vga_write("\n");
}


/* ---------- v2.25 full-screen user administration UI (`useredit`) -------
 * Full-screen takeover: con_screen_push() saves the live grid, the TUI
 * owns every row until Q/Esc, con_screen_pop() restores the shell view
 * verbatim.  Requires CAP_USERADM (or root). */
static void ue_line(int y, int x, const char *txt, uint8_t fg, uint8_t bg) {
    if (y < 0 || y >= con_rows()) return;
    con_set_style(fg, bg, 0);
    con_write_at((uint8_t)x, (uint8_t)y, txt);
}

static void ue_pad(int y, uint8_t bg) {
    char blanks[128];
    int w = con_cols();
    if (w > 120) w = 120;
    for (int i = 0; i < w; i++) blanks[i] = ' ';
    blanks[w] = 0;
    con_set_style(CON_LIGHT_GREY, bg, 0);
    con_write_at(0, (uint8_t)y, blanks);
}

static void ue_frame(const char *title) {
    for (int y = 0; y < con_rows(); y++) ue_pad(y, CON_BLUE);
    ue_line(0, 2, "+-------------------------------------------------------------+",
            CON_YELLOW, CON_BLUE);
    ue_line(1, 2, "|                                                             |",
            CON_YELLOW, CON_BLUE);
    char hdr[80];
    int o = 0;
    for (const char *q = "| "; *q && o < 78; q++) hdr[o++] = *q;
    for (const char *q = title; *q && o < 77; q++) hdr[o++] = *q;
    while (o < 63) hdr[o++] = ' ';
    for (const char *q = "|"; *q && o < 78; q++) hdr[o++] = *q;
    hdr[o] = 0;
    ue_line(1, 2, hdr, CON_YELLOW, CON_BLUE);
}

static const char *ue_pwmark(const user_rec_t *u) {
    return user_pw_is_set(u) ? "yes" : "no ";
}

static void ue_draw_list(int sel) {
    ue_frame("CatOS user administration (v2.25 fine-grained caps)");
    ue_line(2, 4, "  # name        uid gid caps      password",
            CON_WHITE, CON_BLUE);
    char ln[80];
    int row = 3;
    for (int i = 0; i < 10 && row < con_rows() - 3; i++) {
        user_rec_t *u = user_at(i);
        if (!u || !u->used) continue;
        int o = 0;
        ln[o++] = u->uid == (int)sel ? '>' : ' ';
        ln[o++] = ' ';
        char num[12];
        int n = 0;
        int v = i;
        do { num[n++] = (char)('0' + v % 10); v /= 10; } while (v > 0);
        if (n < 2) ln[o++] = '0';
        while (n > 0) ln[o++] = num[--n];
        ln[o++] = ' ';
        for (const char *q = u->name; *q && o < 14; q++) ln[o++] = *q;
        while (o < 16) ln[o++] = ' ';
        n = 0; v = u->uid;
        do { num[n++] = (char)('0' + v % 10); v /= 10; } while (v > 0);
        while (n > 0) ln[o++] = num[--n];
        ln[o++] = ' ';
        n = 0; v = u->gid;
        do { num[n++] = (char)('0' + v % 10); v /= 10; } while (v > 0);
        while (n > 0) ln[o++] = num[--n];
        while (o < 24) ln[o++] = ' ';
        for (int k = 0; k < 8; k++) {
            uint32_t c = user_caps_of(u->uid);
            ln[o++] = "0123456789ABCDEF"[(c >> (28 - k * 4)) & 0xF];
        }
        ln[o++] = ' ';
        for (const char *q = ue_pwmark(u); *q && o < 76; q++) ln[o++] = *q;
        ln[o] = 0;
        ue_line(row, 2, ln,
                u->uid == (int)sel ? CON_BLACK : CON_LIGHT_GREY,
                u->uid == (int)sel ? CON_YELLOW : CON_BLUE);
        row++;
    }
    ue_line(con_rows() - 2, 2,
            " arrows=select  N=new  E=edit caps  P=password  D=delete  Q=quit ",
            CON_BLACK, CON_LIGHT_GREY);
}

static void ue_draw_caps(const char *name, uint32_t caps) {
    ue_frame("editing capabilities (space toggles, R=preset, Esc=done)");
    static const struct { uint32_t bit; const char *q; } W[] = {
        { CAP_NET,     "network (http/dns/udp/ping)" },
        { CAP_KILL,    "kill other users' tasks" },
        { CAP_FSADMIN, "fs admin broad (format+fsck)" },
        { CAP_FORMAT,  "format disks (fine)" },
        { CAP_FSCK,    "fsck repair (fine)" },
        { CAP_AUDIO,   "audio playback" },
        { CAP_USERADM, "user admin" },
        { CAP_DEVICE,  "raw device broad" },
        { CAP_RAWIO,   "raw I/O (fine)" },
        { CAP_HWINFO,  "hw info read (fine)" },
        { CAP_SYS,     "system broad (reboot+diag)" },
        { CAP_REBOOT,  "reboot (fine)" },
        { CAP_DIAG,    "kernel introspection (fine)" },
    };
    char ln[80];
    int row = 3;
    for (int i = 0; i < (int)(sizeof(W) / sizeof(W[0])) && row < con_rows() - 3; i++) {
        int o = 0;
        ln[o++] = '[';
        ln[o++] = (caps & W[i].bit) ? 'x' : ' ';
        ln[o++] = ']';
        ln[o++] = ' ';
        for (const char *q = W[i].q; *q && o < 60; q++) ln[o++] = *q;
        ln[o] = 0;
        ue_line(row, 4, ln, (caps & W[i].bit) ? CON_LIGHT_GREEN : CON_LIGHT_GREY,
                CON_BLUE);
        row++;
    }
    char st[80];
    int o = 0;
    for (const char *q = name; *q && o < 30; q++) st[o++] = *q;
    for (const char *q = "  caps=0x"; *q && o < 62; q++) st[o++] = *q;
    for (int k = 0; k < 8 && o < 70; k++)
        st[o++] = "0123456789ABCDEF"[(caps >> (28 - k * 4)) & 0xF];
    st[o] = 0;
    ue_line(con_rows() - 2, 2, st, CON_BLACK, CON_LIGHT_GREY);
}

static void ue_edit_caps(const char *name) {
    user_rec_t *u = user_by_name(name);
    if (!u) return;
    uint32_t caps = user_caps_of(u->uid);
    static const uint32_t bits[] = {
        CAP_NET, CAP_KILL, CAP_FSADMIN, CAP_FORMAT, CAP_FSCK, CAP_AUDIO,
        CAP_USERADM, CAP_DEVICE, CAP_RAWIO, CAP_HWINFO, CAP_SYS,
        CAP_REBOOT, CAP_DIAG
    };
    int nb = (int)(sizeof(bits) / sizeof(bits[0]));
    int cur = 0;
    for (;;) {
        ue_draw_caps(name, caps);
        int k = kb_getkey();
        if (k == KB_UP)   { if (--cur < 0) cur = nb - 1; }
        else if (k == KB_DOWN) { if (++cur >= nb) cur = 0; }
        else if (k == ' ') caps ^= bits[cur];
        else if (k == 27 || k == '\n') {
            user_set_caps(name, caps);
            if (user_save() != 0) vga_write("useredit: persist failed\n");
            return;
        }
        else if (k == 'r' || k == 'R') {
            ue_line(con_rows() - 1, 2,
                    " preset: admin/operator/developer/viewer/guest: ",
                    CON_WHITE, CON_BLUE);
            char p[16];
            kb_readline(p, sizeof(p));
            uint32_t pc;
            if (p[0] && user_preset_caps(p, &pc) == 0) caps = pc;
        }
    }
}

static void ue_password(const char *name) {
    char p1[64], p2[64];
    ue_line(con_rows() - 1, 2, " new password (masked): ", CON_WHITE, CON_BLUE);
    kb_readline_masked(p1, sizeof(p1));
    ue_line(con_rows() - 1, 2, " retype:                 ", CON_WHITE, CON_BLUE);
    kb_readline_masked(p2, sizeof(p2));
    if (!p1[0] || strcmp_(p1, p2) != 0) {
        ue_line(con_rows() - 1, 2, " password NOT set (empty or mismatch) ",
                CON_LIGHT_RED, CON_BLUE);
        return;
    }
    user_set_pw(name, p1);
    if (user_save() != 0)
        ue_line(con_rows() - 1, 2, " persist FAILED ", CON_LIGHT_RED, CON_BLUE);
    else
        ue_line(con_rows() - 1, 2, " password set + persisted ", CON_LIGHT_GREEN, CON_BLUE);
}

static void cmd_useredit(void) {
    if (!session_is_admin()) {
        vga_write("useredit: permission denied (need CAP_USERADM)\n");
        return;
    }
    if (con_screen_push() != 0) { vga_write("useredit: push failed\n"); return; }
    int sel = UID_USER;
    for (;;) {
        ue_draw_list(sel);
        int k = kb_getkey();
        if (k == KB_UP) {
            int best = -1;
            for (int i = 0; i < 10; i++) {
                user_rec_t *u = user_at(i);
                if (u && u->used && u->uid < sel && u->uid > best) best = u->uid;
            }
            if (best >= 0) sel = best;
        } else if (k == KB_DOWN) {
            int best = -1;
            for (int i = 0; i < 10; i++) {
                user_rec_t *u = user_at(i);
                if (u && u->used && u->uid > sel &&
                    (best < 0 || u->uid < best)) best = u->uid;
            }
            if (best >= 0) sel = best;
        } else if (k == 'q' || k == 'Q' || k == 27) break;
        else if (k == 'e' || k == 'E') {
            user_rec_t *u = user_by_uid(sel);
            if (u && u->uid != UID_ROOT) ue_edit_caps(u->name);
        } else if (k == 'p' || k == 'P') {
            user_rec_t *u = user_by_uid(sel);
            if (u) ue_password(u->name);
        } else if (k == 'd' || k == 'D') {
            user_rec_t *u = user_by_uid(sel);
            if (u && u->uid != UID_ROOT) {
                user_del(u->name);
                user_save();
                sel = UID_ROOT;
            }
        } else if (k == 'n' || k == 'N') {
            ue_line(con_rows() - 1, 2, " new user name: ", CON_WHITE, CON_BLUE);
            char nm[24], uid[8];
            kb_readline(nm, sizeof(nm));
            if (nm[0] && !user_by_name(nm)) {
                ue_line(con_rows() - 1, 2, " uid (>=100):   ", CON_WHITE, CON_BLUE);
                kb_readline(uid, sizeof(uid));
                int uidv = uid[0] ? atoi_(uid) : 101;
                if (uidv < 100) uidv = 101;
                char home[48];
                int o = 0;
                for (const char *q = "/home/"; *q && o < 47; q++) home[o++] = *q;
                for (const char *q = nm; *q && o < 47; q++) home[o++] = *q;
                home[o] = 0;
                user_add(nm, uidv, uidv, home, "/bin/shell.ce", 0);
                user_save();
                sel = uidv;
            }
        }
    }
    con_screen_pop();
}

/* ---------- shell loop ---------- */
/* v2.33: shell aliases (bash-style, single-level expansion) */
#define ALIAS_MAX 8
static char al_name[ALIAS_MAX][16];
static char al_value[ALIAS_MAX][64];
static int  al_count;

static int alias_lookup(const char *n) {
    for (int i = 0; i < al_count; i++)
        if (!strcmp_(al_name[i], n)) return i;
    return -1;
}

static void cmd_alias(char *arg) {
    char *eq = 0;
    for (char *p = arg; *p; p++) if (*p == '=') { eq = p; break; }
    if (!eq) {                          /* list all (or one) */
        if (*arg) {
            int i = alias_lookup(arg);
            if (i < 0) { vga_write("alias: not found: "); vga_write(arg); vga_write("\n"); }
            else { vga_write("alias "); vga_write(al_name[i]); vga_write("='");
                   vga_write(al_value[i]); vga_write("'\n"); }
            return;
        }
        for (int i = 0; i < al_count; i++) {
            vga_write("alias "); vga_write(al_name[i]); vga_write("='");
            vga_write(al_value[i]); vga_write("'\n");
        }
        if (!al_count) vga_write("(no aliases)\n");
        return;
    }
    *eq = 0;
    char *name = arg, *value = eq + 1;
    if (!*name || !*value) { vga_write("usage: alias name=value\n"); return; }
    int l = 0; while (name[l]) l++;
    if (l >= 16) { vga_write("alias: name too long\n"); return; }
    int i = alias_lookup(name);
    if (i < 0) {
        if (al_count >= ALIAS_MAX) { vga_write("alias: table full\n"); return; }
        i = al_count++;
        for (int j = 0; j < l; j++) al_name[i][j] = name[j];
        al_name[i][l] = 0;
    }
    if (strcat_b(al_value[i], 64, value) != 0) {   /* overwrite in place */
        int v = 0; while (value[v] && v < 63) { al_value[i][v] = value[v]; v++; }
        al_value[i][v] = 0;
    }
}

static void cmd_unalias(char *arg) {
    if (!*arg) { vga_write("usage: unalias name\n"); return; }
    int i = alias_lookup(arg);
    if (i < 0) { vga_write("unalias: not found: "); vga_write(arg); vga_write("\n"); return; }
    for (int j = i; j < al_count - 1; j++) {
        for (int k = 0; k < 16; k++) al_name[j][k] = al_name[j + 1][k];
        for (int k = 0; k < 64; k++) al_value[j][k] = al_value[j + 1][k];
    }
    al_count--;
}

static char input[256];


/* ---- v2.28: TAB completion --------------------------------------------
 * helpers: bounded strncmp and bounded string append */
static int sh_strneq_(const char *s, const char *p, int n) {
    for (int i = 0; i < n; i++) {
        if (s[i] != p[i]) return 0;
        if (!s[i]) return 1;              /* prefix already shorter */
    }
    return 1;
}
static void bar_put_str(char *b, int *n, const char *s) {
    while (*s && *n < 158) b[(*n)++] = *s++;
}
/* ---- v2.28: TAB completion ----
 * token 0 completes against the command table; later tokens complete
 * against the current directory's file names (prefix only, no path
 * parsing).  Multiple matches go into the candidate bar (the row under
 * the input line), exactly one replacement goes back into the line. */
static const char *sh_cmds[] = {
    "about","alias","autostart","caps","cat","cd","chcaps","chmod","chown","clear",
    "color","cp","curl","date","dir","display","dns","echo","edit","env",
    "export","faultinfo","format","fsck","gfx","gfx24","hang","help","hwinfo",
    "history","id","jobs","kill","lang","log","login","ls","lsuser","mem","mkdir","mv",
    "netcfg","netdiag","palette","passwd","ping","ps","reboot","rgb","rm","run",
    "setenv","size","sndinfo","sndtest","style","sudo","svgainfo","text","top",
    "touch","udp","uname","unalias","unset","uptime","usb","useradd","userdel","useredit",
    "users","vbe","vbe24","vdiag","whoami",
};
#define SH_NCMDS ((int)(sizeof(sh_cmds) / sizeof(sh_cmds[0])))

static void sh_best_prefix(const char *const *tab, int n,
                           const char *pfx, int plen,
                           char *out, char *bar) {
    out[0] = 0; bar[0] = 0;
    const char *hit = 0; int hits = 0;
    for (int i = 0; i < n; i++)
        if (plen == 0 || (sh_strneq_(tab[i], pfx, plen))) { hit = tab[i]; hits++; }
    if (hits == 1) {
        int j = 0;
        while (hit[j] && j < 120) out[j] = hit[j], j++;
        out[j++] = ' '; out[j] = 0;
        return;
    }
    if (hits > 1) {                       /* candidate bar, first N names */
        int n2 = 0;
        bar_put_str(bar, &n2, "[tab] ");
        for (int i = 0; i < n && n2 < 140; i++)
            if (plen == 0 || sh_strneq_(tab[i], pfx, plen)) {
                bar_put_str(bar, &n2, tab[i]);
                bar_put_str(bar, &n2, " ");
            }
        bar[n2] = 0;
    }
}

static int sh_complete(const char *line, int cur, char out[KB_BUF], char bar[160]) {
    /* command token = the region from the first non-blank char up to the
     * first blank; if `cur` is inside it (or nothing typed yet) complete
     * against the command table, otherwise against file names. */
    int s0 = 0;
    while (s0 < cur && line[s0] == ' ') s0++;
    int blank = (s0 >= cur);
    int cmd_end = s0;
    while (cmd_end < cur && line[cmd_end] != ' ') cmd_end++;
    if (blank || cmd_end >= cur) {        /* inside the command token */
        int plen = cur - s0;
        if (plen == 0) { out[0] = 0; bar[0] = 0; return 0; }  /* no hint on bare TAB */
        sh_best_prefix(sh_cmds, SH_NCMDS, line + s0, plen, out, bar);
        return (out[0] != 0);
    }
    int ts = s0;
    int i = cmd_end;
    while (i < cur) {
        while (i < cur && line[i] == ' ') i++;
        if (i >= cur) break;
        ts = i;
        while (i < cur && line[i] != ' ') i++;
        if (i >= cur) break;
    }
    int te = (i > cur) ? cur : i;
    if (ts >= te) { out[0] = 0; bar[0] = 0; return 0; }
    /* path completion only for a plain name (no '/' yet) */
    for (int j = ts; j < te; j++)
        if (line[j] == '"') { out[0] = 0; bar[0] = 0; return 0; }
    static char names[64][FS_NAME_MAX];
    char cwdp[FS_PATH_MAX];
    fs_pwd(cwdp, sizeof(cwdp));
    int n = fs_enum_entries(cwdp, names, 64);
    if (n <= 0) { out[0] = 0; bar[0] = 0; return 0; }
    /* shell order: files enum returns dir names first?  just prefix-match */
    int plen = te - ts;
    int kept = 0;
    static const char *ptrs[64];
    static char flat[64][FS_NAME_MAX];
    for (int j = 0; j < n; j++)
        if (plen == 0 || sh_strneq_(names[j], line + ts, plen)) {
            for (int k2 = 0; k2 < FS_NAME_MAX; k2++) flat[kept][k2] = names[j][k2];
            ptrs[kept] = flat[kept];
            kept++;
        }
    if (kept == 0) { out[0] = 0; bar[0] = 0; return 0; }
    char fo[KB_BUF], fbar[160];
    sh_best_prefix(ptrs, kept, line + ts, plen, fo, fbar);
    if (fo[0]) {                          /* splice the completed token */
        int j = 0, o = 0;
        for (; j < ts && o < KB_BUF - 1; j++) out[o++] = line[j];
        for (j = 0; fo[j] && o < KB_BUF - 1; j++) out[o++] = fo[j];
        for (j = te; line[j] && o < KB_BUF - 1; j++) out[o++] = line[j];
        out[o] = 0;
        if (fbar[0]) { int b = 0; while (fbar[b] && b < 159) { bar[b] = fbar[b]; b++; } bar[b] = 0; }
        return 1;
    }
    if (fbar[0]) { int b = 0; while (fbar[b] && b < 159) { bar[b] = fbar[b]; b++; } bar[b] = 0; }
    return 0;
}

static void prompt(void) {
    /* v2.25: keep the input line on the middle row of the screen. */
    con_scroll_anchor();
    char pwd[160];
    fs_pwd(pwd, sizeof(pwd));
    user_rec_t *u = user_by_uid(g_uid);
    vga_set_fg(VGA_LIGHT_GREEN);
    if (u) { vga_write(u->name); vga_write("@"); }
    vga_write("catos");
    vga_set_fg(VGA_LIGHT_BLUE);
    vga_write(":");
    vga_write(pwd);
    vga_write("$ ");
    vga_set_fg(VGA_WHITE);
}

/* show the NIC state on the boot screen (kernel's net_init line is
 * cleared by vga_clear, so this re-prints it for the user) */
static void net_boot_status(void) {
    extern int net_ready;
    extern int netdbg_pci_count;
    extern uint16_t netdbg_pci_vend[8];
    extern uint16_t netdbg_pci_dev[8];
    extern int net_fail_code;
    extern uint8_t net_mac[6];
    extern int net_env_vmware(void);
    if (net_ready) {
        vga_write("net: AMD PCnet-PCI up  mac=");
        print_hex((net_mac[0]<<8)|net_mac[1]); vga_write(":");
        print_hex((net_mac[2]<<8)|net_mac[3]); vga_write(":");
        print_hex((net_mac[4]<<8)|net_mac[5]); vga_write("\n");
        return;
    }
    vga_write("net: no usable NIC");
    if (net_fail_code == 1) vga_write(" (no 1022:2000)");
    else if (net_fail_code == 2) vga_write(" (open bus)");
    else if (net_fail_code == 3) vga_write(" (MAC unreadable)");
    else if (net_fail_code == 4) vga_write(" (INIT/START failed)");
    vga_write("; PCI bus 0: ");
    if (netdbg_pci_count == 0) {
        vga_write("no devices\n");
    } else {
        for (int i = 0; i < netdbg_pci_count; i++) {
            vga_write("vend="); print_hex(netdbg_pci_vend[i]);
            vga_write(" dev="); print_hex(netdbg_pci_dev[i]);
            if (i < netdbg_pci_count - 1) vga_write(" ");
        }
        vga_write("\n");
    }
    if (net_env_vmware())
        vga_write("net: VMware guest -> set the VM NIC to Intel E1000\n"
                  "     (PCnet path works only when the adapter is 'AMD PCnet')\n");
    else
        vga_write("net: QEMU -> -netdev user,id=n0 -device pcnet,netdev=n0\n");
}

void shell_run(void) {
    mbi = (multiboot_info_t *)multiboot_info_ptr;

    vga_clear();
    fs_init();
    lang_init();              /* v2.21 T50: load /etc/lang, default en */
    env_init();               /* v2.23: load /etc/env, default PATH=/bin */
    ce_init();
    /* v2.13: load the persistent user database + ACL (seeds root/user and
     * the default owner/group/other table when the files are absent) */
    user_db_init();
    acl_init();
    session_apply();                /* boot session = root (initial user) */
    if (!fs_fs_ready())
        vga_write(tr(L_NO_FAT32));
    else {
        ce_install_builtins();      /* fallback: install built-in apps at boot */
        /* v2.17: boot output now comes from /etc/autostart scripts
         * (echo lines) instead of a hardcoded banner.  Falls back to a
         * seeded default script on a fresh volume. */
        boot_autostart();
    }

    kb_set_completer(sh_complete);        /* v2.28: TAB completion */

    while (1) {
        prompt();
        if (kb_readline(input, sizeof(input)) == 2) {   /* ^C cancels */
            vga_set_fg(VGA_LIGHT_GREEN);
            vga_write("^C\n");
            vga_set_fg(VGA_WHITE);
            continue;
        }
        env_expand(input, (int)sizeof(input));   /* v2.23: $KEY / ${KEY} */

        /* Split first token */
        char *cmd = input;
        while (*cmd == ' ') cmd++;
        char *arg = cmd;
        while (*arg && *arg != ' ') arg++;
        if (*arg) { *arg = 0; arg++; while (*arg == ' ') arg++; }

        if (!*cmd) continue;

        /* v2.33: single-level alias expansion of the command word */
        int ai = alias_lookup(cmd);
        if (ai >= 0) {
            char expanded[256];
            expanded[0] = 0;
            strcat_b(expanded, (int)sizeof(expanded), al_value[ai]);
            if (*arg && strcat_b(expanded, (int)sizeof(expanded), " ") == 0)
                strcat_b(expanded, (int)sizeof(expanded), arg);
            char *earg = expanded;
            while (*earg && *earg != ' ') earg++;
            if (*earg) { *earg = 0; earg++; while (*earg == ' ') earg++; }
            shell_exec(expanded, earg);
            continue;
        }
        shell_exec(cmd, arg);
    }
}

/* Execute one parsed command line: builtins first, then current-dir
 * program and /bin PATH lookup.  Shared by the interactive loop and the
 * /etc/autostart script runner (identical semantics for both). */
static void shell_exec(char *cmd, char *arg) {
        if (!strcmp_(cmd, "help"))            cmd_help();
        else if (!strcmp_(cmd, "lang"))      cmd_lang(arg);
        else if (!strcmp_(cmd, "export") ||
                 !strcmp_(cmd, "setenv"))    cmd_export(arg);
        else if (!strcmp_(cmd, "env"))       env_list();
        else if (!strcmp_(cmd, "unset"))     cmd_unset(arg);
        else if (!strcmp_(cmd, "alias"))     cmd_alias(arg);
        else if (!strcmp_(cmd, "unalias"))   cmd_unalias(arg);
        else if (!strcmp_(cmd, "clear"))      vga_clear();
        else if (!strcmp_(cmd, "gfx"))        cmd_gfx();
        else if (!strcmp_(cmd, "vbe"))        cmd_vbe();
        else if (!strcmp_(cmd, "gfx24") ||
                 !strcmp_(cmd, "vbe24"))      cmd_gfx24();
        else if (!strcmp_(cmd, "svgainfo"))   cmd_svgainfo();
        else if (!strcmp_(cmd, "text"))       cmd_text();
        else if (!strcmp_(cmd, "style"))      cmd_style(arg);
        else if (!strcmp_(cmd, "rgb"))        cmd_rgb(arg);
        else if (!strcmp_(cmd, "palette"))    cmd_palette(arg);
        else if (!strcmp_(cmd, "display"))    cmd_display();
        else if (!strcmp_(cmd, "size"))       cmd_size(arg);
        else if (!strcmp_(cmd, "useredit"))   cmd_useredit();
        else if (!strcmp_(cmd, "usb")) {
            if (!strcmp_(arg, "scan")) {
                int n = usb_scan_devices();
                vga_write(n ? "usb: keyboard attached\n"
                            : "usb: no device found\n");
            } else {
                char b[96];
                vga_write("usb: "); vga_write(usb_status(b, sizeof(b)));
                vga_write("\n");
            }
        }
        else if (!strcmp_(cmd, "vdiag"))      con_diag();
        else if (!strcmp_(cmd, "echo"))      { vga_write(arg); vga_write("\n"); }
        else if (!strcmp_(cmd, "about"))      cmd_about();
        else if (!strcmp_(cmd, "autostart"))  boot_autostart();
        else if (!strcmp_(cmd, "uname"))      cmd_uname();
        else if (!strcmp_(cmd, "date"))       cmd_date();
        else if (!strcmp_(cmd, "mem"))        cmd_mem();
        else if (!strcmp_(cmd, "reboot")) {
            /* v2.25 fine-grained split: CAP_SYS (broad) or CAP_REBOOT. */
            if (g_uid == UID_ROOT || (g_caps & (CAP_SYS | CAP_REBOOT)))
                cmd_reboot();
            else
                vga_write("reboot: permission denied (need CAP_SYS or CAP_REBOOT)\n");
        }
        else if (!strcmp_(cmd, "hang"))      cmd_hang();
        else if (!strcmp_(cmd, "dir") || !strcmp_(cmd, "ls")) fs_list(*arg ? arg : 0);
        else if (!strcmp_(cmd, "cd"))         cmd_cd(arg);
        else if (!strcmp_(cmd, "mkdir")) {
            if (arg[0] == '-' && arg[1] == 'p' && (arg[2] == ' ' || arg[2] == 0)) {
                const char *pa = arg + 2;
                while (*pa == ' ') pa++;
                cmd_mkdir_p(pa, 1);
            } else               cmd_mkdir_p(arg, 0);
        }
        else if (!strcmp_(cmd, "touch"))      cmd_touch(arg);
        else if (!strcmp_(cmd, "cat"))        cmd_cat(arg);
        else if (!strcmp_(cmd, "rm"))         cmd_rm(arg);
        else if (!strcmp_(cmd, "cp"))         cmd_cp(arg);
        else if (!strcmp_(cmd, "mv"))         cmd_mv(arg);
        else if (!strcmp_(cmd, "format")) {
            /* v2.25: fine-grained split -- CAP_FSADMIN (broad) or the
             * new CAP_FORMAT successor both grant mkfs. */
            if (session_is_fsadmin() || (g_caps & CAP_FORMAT)) cmd_format();
            else vga_write("format: permission denied (need CAP_FSADMIN or CAP_FORMAT)\n");
        }
        else if (!strcmp_(cmd, "edit"))      cmd_edit(arg);
        else if (!strcmp_(cmd, "run"))        cmd_run(arg, g_uid);
        else if (!strcmp_(cmd, "sudo")) {
            /* sudo CMD ...: run as root; requires CAP_USERADM (or root) */
            if (!session_is_admin()) {
                vga_write("sudo: permission denied (need CAP_USERADM)\n");
            } else {
            while (*arg == ' ') arg++;
            char *scmd = arg;
            char *sarg = arg;
            while (*sarg && *sarg != ' ') sarg++;
            if (*sarg) { *sarg = 0; sarg++; while (*sarg == ' ') sarg++; }
            if (!*scmd) { vga_write("usage: sudo COMMAND [args...]\n"); }
            else if (!strcmp_(scmd, "run"))   cmd_run(sarg, UID_ROOT);
            else {
                char *argv[16];
                int argc = 1;
                argv[0] = scmd;
                argc += split_words(sarg, argv + 1, 15);
                if (run_from_path(scmd, argv, argc, 0, UID_ROOT) != 0) {
                    vga_write("catos: unknown command: ");
                    vga_write(scmd); vga_write("\n");
                }
            }
            }
        }
        else if (!strcmp_(cmd, "whoami"))      cmd_whoami();
        else if (!strcmp_(cmd, "id"))          cmd_id(arg);
        else if (!strcmp_(cmd, "lsuser") ||
                 !strcmp_(cmd, "users"))       cmd_lsuser();
        else if (!strcmp_(cmd, "caps"))        cmd_caps_legend();
        else if (!strcmp_(cmd, "login"))       cmd_login(arg, 0);
        else if (!strcmp_(cmd, "su"))          cmd_login(arg, 1);
        else if (!strcmp_(cmd, "useradd"))     cmd_useradd(arg);
        else if (!strcmp_(cmd, "userdel"))     cmd_userdel(arg);
        else if (!strcmp_(cmd, "passwd"))      cmd_passwd(arg);
        else if (!strcmp_(cmd, "chcaps"))      cmd_chcaps(arg);
        else if (!strcmp_(cmd, "chmod"))       cmd_chmod(arg);
        else if (!strcmp_(cmd, "chown"))       cmd_chown(arg);
        else if (!strcmp_(cmd, "history")) {
            int n = kb_hist_count();
            for (int i = 0; i < n; i++) {
                vga_write("  ");
                print_int(i + 1);
                vga_write("  ");
                vga_write(kb_hist_get(i));
                vga_write("\n");
            }
        }
        else if (!strcmp_(cmd, "ps"))          task_ps_print();
        else if (!strcmp_(cmd, "top"))         cmd_top();
        else if (!strcmp_(cmd, "jobs"))        cmd_jobs();
        else if (!strcmp_(cmd, "log"))         cmd_log(arg);
        else if (!strcmp_(cmd, "hwinfo"))      cmd_hwinfo();
        else if (!strcmp_(cmd, "sndinfo"))     cmd_sndinfo();
        else if (!strcmp_(cmd, "sndtest"))     cmd_sndtest(arg);
        else if (!strcmp_(cmd, "netcfg"))       cmd_netcfg(arg);
        else if (!strcmp_(cmd, "uptime")) {
            print_int((int)(task_uptime_ms() / 1000));
            vga_write("s (");
            print_int((int)task_count());
            vga_write(" tasks)\n");
        }
        else if (!strcmp_(cmd, "kill")) {
            int tid = 0, neg = 0;
            while (*arg == ' ') arg++;
            if (*arg == '-') { neg = 1; arg++; }
            while (*arg >= '0' && *arg <= '9') { tid = tid * 10 + (*arg - '0'); arg++; }
            if (neg) tid = -tid;
            if (tid == 0) { vga_write("usage: kill TID\n"); }
            else if (task_kill(tid, UID_ROOT) != 0)
                vga_write("kill: no such task\n");
        }
        else if (!strcmp_(cmd, "curl"))        cmd_curl(arg);
        else if (!strcmp_(cmd, "ping"))        cmd_ping(arg);
        else if (!strcmp_(cmd, "dns"))         cmd_dns(arg);
        else if (!strcmp_(cmd, "udp"))         cmd_udp(arg);
        else if (!strcmp_(cmd, "netdiag"))     cmd_netdiag(arg);
        else if (!strcmp_(cmd, "faultinfo"))   faultinfo_print();
        else if (!strcmp_(cmd, "fsck")) {
            if (!(session_is_fsadmin() || (g_caps & CAP_FSCK))) {
                vga_write("fsck: permission denied (need CAP_FSADMIN or CAP_FSCK)\n");
            } else {
            int r = fat_verify_mirror();
            if (r < 0)      vga_write("fsck: ATA error while comparing FAT mirrors\n");
            else if (r == 0) vga_write("fsck: FAT mirror OK\n");
            else {
                vga_write("fsck: repaired "); print_int(r);
                vga_write(" FAT sector(s) from mirror #1\n");
            }
            }
        }
        else if (!strcmp_(cmd, "color")) {
            if (!*arg) {
                vga_write("usage: color 0..255  (text mode maps to 16 colors)\n");
            } else {
                int c = 0;
                while (*arg >= '0' && *arg <= '9') { c = c * 10 + (*arg - '0'); arg++; }
                if (c >= 0 && c <= 255) con_set_fg((uint8_t)c);
                else vga_write("usage: color 0..255\n");
            }
        }
        else {
            /* not a builtin: current-dir program, then /bin (PATH), else
             * report an unknown command.  Runs as uid USER (use 'sudo'
             * for root).  A trailing '&' backgrounds the task.  v2.5:
             * `cmd [> F | >> F | | CMD]` redirection is supported. */
            char *out_redir = 0, *pipe_to = 0; int append = 0;
            parse_redir(arg, &out_redir, &append, &pipe_to);
            char *argv[16];
            int argc = 1;
            argv[0] = cmd;
            argc += split_words(arg, argv + 1, 15);
            int bg = 0;
            if (argc > 1 && argv[argc - 1][0] == '&' && argv[argc - 1][1] == 0) {
                bg = 1;
                argc--;
            }
            if (out_redir || pipe_to) {
                /* resolve the left-hand command via PATH, then spawn with
                 * redirection wired (run_from_path cannot wire it) */
                char p[FS_PATH_MAX];
                char pwd[FS_PATH_MAX];
                fs_pwd(pwd, sizeof(pwd));
                uint32_t c = 0, s = 0;
                p[0] = 0;
                if (!(pwd[0] == '/' && pwd[1] == 0)) {
                    if (strcat_b(p, sizeof(p), pwd) == 0) strcat_b(p, sizeof(p), "/");
                } else strcat_b(p, sizeof(p), "/");
                if (p[0]) strcat_b(p, sizeof(p), cmd);
                if (p[0]) strcat_b(p, sizeof(p), cmd_has_ext(cmd) ? "" : ".ce");
                if (p[0] && fs_resolve_file(p, &c, &s) != 0) {
                    p[0] = 0;
                    if (strcat_b(p, sizeof(p), "/bin/") == 0 &&
                        strcat_b(p, sizeof(p), cmd) == 0 &&
                        strcat_b(p, sizeof(p), cmd_has_ext(cmd) ? "" : ".ce") == 0 &&
                        fs_resolve_file(p, &c, &s) != 0) p[0] = 0;
                }
                if (!p[0]) {
                    vga_write("catos: unknown command: "); vga_write(cmd); vga_write("\n");
                } else if (pipe_to && *pipe_to) {
                    char *file2 = pipe_to;
                    char *sp = pipe_to;
                    while (*sp && *sp != ' ') sp++;
                    if (*sp) { *sp = 0; sp++; }
                    char *argv2[16];
                    int argc2 = 1;
                    argv2[0] = file2;
                    argc2 += split_words(sp, argv2 + 1, 15);
                    int h = kpipe_create();
                    if (h < 0) { vga_write("run: no pipe slots\n"); }
                    else {
                        vga_clear();
                        spawn_ce_redirect(p, argc, argv, 0, g_uid, 0, 0, h, -1);
                        spawn_ce_redirect(file2, argc2, argv2, 1, g_uid, 0, 0, -1, h);
                    }
                } else {
                    vga_clear();
                    spawn_ce_redirect(p, argc, argv, !bg, g_uid,
                                      out_redir, append, -1, -1);
                }
            } else if (run_from_path(cmd, argv, argc, bg, g_uid) != 0) {
                vga_write("catos: unknown command: ");
                vga_write(cmd); vga_write("\n");
            }
        }
}

/* ---------- v2.17 boot autostart (/etc/autoexec) ---------- */

/* 8.3 names only: the volume has no LFN support, so "autostart" (9-char
 * stem) is rejected by path_has_long_names() -- hence "autoexec". */
#define AUTOSTART_DIR "/etc/autoexec"

/* Seeded on a fresh volume so the boot banner still works out of the
 * box; the user may edit or delete it and add more scripts. */
static const char *const AUTOSTART_DEFAULT =
    "# CatOS boot scripts live in /etc/autoexec\n"
    "# (one shell command per line, '#' comments and blank lines skipped,\n"
    "#  8.3 file names -- the volume has no long-name support).\n"
    "echo CatOS 0.3 -- A tiny Unix-like OS on GRUB Multiboot\n"
    "echo Architecture : i386 (32-bit x86)\n"
    "echo Bootloader   : GRUB 2 (ISO9660 / El Torito)\n"
    "echo Filesystem   : FAT32 on IDE (persistent)\n"
    "echo Display      : front-end + text/gfx back-ends\n"
    "echo Runs on      : QEMU / VMware / Bochs / real PC\n"
    "echo Type 'help' for a list of commands.\n";

static void boot_autostart(void) {
    if (!fs_fs_ready()) return;
    /* seed dir + default banner (fs_write_raw skips an existing file) */
    fs_mkdir(AUTOSTART_DIR);
    fs_write_raw(AUTOSTART_DIR "/banner.sh", AUTOSTART_DEFAULT,
                 (uint32_t)slen_(AUTOSTART_DEFAULT));

    char names[8][FS_NAME_MAX];
    int n = fs_enum_files(AUTOSTART_DIR, names, 8);
    if (n <= 0) return;
    for (int i = 0; i < n && i < 8; i++) {
        char path[FS_PATH_MAX];
        int k = 0;
        const char *s;
        for (s = AUTOSTART_DIR; *s && k < (int)sizeof(path) - 1; s++)
            path[k++] = *s;
        if (k < (int)sizeof(path) - 1) path[k++] = '/';
        for (s = names[i]; *s && k < (int)sizeof(path) - 1; s++)
            path[k++] = *s;
        path[k] = 0;

        static char buf[2048];
        int len = fs_read_text(path, buf, (int)sizeof(buf) - 1);
        if (len <= 0) continue;
        buf[len] = 0;

        vga_write("autostart: ");
        vga_write(names[i]);
        vga_write("\n");
        char *p = buf;
        while (*p) {
            char *line = p;
            while (*p && *p != '\n') p++;
            if (*p) *p++ = 0;
            char *e = line + slen_(line);
            if (e > line && e[-1] == '\r') e[-1] = 0;   /* CRLF safety */
            env_expand(line, 256);                      /* v2.23 */

            char *lcmd = line;
            while (*lcmd == ' ' || *lcmd == '\t') lcmd++;
            if (!*lcmd || *lcmd == '#') continue;
            char *larg = lcmd;
            while (*larg && *larg != ' ' && *larg != '\t') larg++;
            if (*larg) {
                *larg = 0; larg++;
                while (*larg == ' ' || *larg == '\t') larg++;
            }
            shell_exec(lcmd, larg);
        }
    }
    net_boot_status();              /* keep the one-line NIC status at boot */
    vga_write("\n");
}
