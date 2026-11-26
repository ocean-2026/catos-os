/*
 * ce_abi.h -- CatOS Executable (CE) user-mode ABI, kernel v2.5.
 *
 * The kernel installs a pointer to this table into the CE image's
 * __ce_api slot before jumping to ce_main(), so a program just does:
 *
 *     catos_api_t *__ce_api __attribute__((section(".ce_api")));
 *     #define CE_API() (__ce_api)
 *
 * Slots are APPEND-ONLY: every new kernel revision appends at the end so
 * that binaries built against an older header keep working unchanged.
 * Never reorder or remove a field.
 *
 * Layout history (slot index = 0-based position in the table):
 *   v1.x  slots  0..32   console / fs / mem / strings / network
 *   v2.3  slots 33..36   getuid / gettid / kill / yield
 *   v2.4  slots 37..38   hw_num / hw_str
 *   v2.5  slots 39..47   big files / pipes / processes
 *   v2.6  slot   48     brk (dynamic heap break)
 *   v2.7  slots 49..51   display mode / display info / text style
 *   v2.8  slots 52..54   RGB style / palette set / palette query
 *   v2.10 slots 55..56   anonymous mmap / munmap
 *   v2.11 slots 57..60   SB16 PCM audio (info / play / stop / tone)
 *   v2.12 slots 61..63   24bpp truecolour drawing (info/rect/blit)
 *   v2.14 slot  64       pollkey (non-blocking key read; games/animations)
 *   v2.21 slot  65       gotoxy (line addressing for TUI rewrite)
 *   v2.69 slots 66..68   mouse x / y / buttons
 *   v2.81 slots 69..71   isolated canvas create / present / destroy
 *
 * Total: 72 slots in v2.81.
 *
 * v2.7/v2.8 deliberately expose NO drawing API: graphics and VBE modes
 * are styled TEXT terminals.  There is no putpixel/fillrect/framebuffer
 * syscall, and neither 0xA0000 nor the VBE LFB is mapped into any ring3
 * page directory.
 *
 * Keep in sync with include/ce.h (the kernel-side mirror).  A v2.4 header
 * has only 40 slots and must NOT be used to build v2.5 programs.
 */
#ifndef CE_ABI_H
#define CE_ABI_H

#include <stdint.h>

/* ---- image header (20 bytes, prepended by mkce.py) ---- */
/* ---- v2.12: 24bpp drawing descriptors --------------------------------
 * The generic syscall thunk passes at most FOUR arguments (ebx/ecx/edx/
 * esi); edi is NOT forwarded.  Instead of extending the hand-packed
 * thunk bytes, the two multi-argument drawing calls take ONE descriptor
 * pointer.  The kernel copies the descriptor and the pixel data with
 * copy_from_user(), so a ring3 caller never writes to VRAM directly.
 * These types are mirrored verbatim in the kernel-side ce.h. */
#ifndef CATOS_GFX_TYPES_H
#define CATOS_GFX_TYPES_H
typedef struct {
    int32_t  x, y, w, h;
    uint32_t rgb;               /* 0x00RRGGBB */
} gfx_rect_t;
typedef struct {
    const uint8_t *src;         /* w*h*3 tightly packed RGB24 bytes */
    int32_t  x, y, w, h;
} gfx_blit_t;
#endif

/* ---- v2.81: isolated canvas channel (user-visible mirror) --------------
 * create() hands the app a writable buffer in its OWN extension arena
 * (anonymous mmap; stride = w*3 tightly packed RGB24).  The kernel keeps a
 * private backing copy; present() pushes a dirty sub-rectangle across.
 * The app never receives a VRAM or compositor surface pointer. */
#ifndef CATOS_CV_TYPES_H
#define CATOS_CV_TYPES_H
typedef struct {
    int32_t  res;               /* >=0 kernel resource handle; <0 on failure */
    int32_t  w, h, stride;      /* stride = w*3 RGB24 bytes per row          */
    void    *buf;                /* app-arena mmap buffer (ring3 writable)    */
} ce_canvas_t;

typedef struct {
    int32_t  res;
    int32_t  x, y, w, h;         /* dirty sub-rectangle inside the canvas    */
} cv_present_t;
#endif

/* ---- P3: declarative channel (user-visible mirror of kernel ce.h) ------- */
#ifndef CATOS_DECL_TYPES_H
#define CATOS_DECL_TYPES_H
typedef struct {
    int32_t  id;
    int32_t  parent;
    uint8_t  kind;
    uint8_t  pad0;
    uint16_t layer;
    int32_t  req_w, req_h;
    int32_t  text_off;
    int32_t  pix_off;
    int32_t  pix_w, pix_h;
    int32_t  res;
} ce_blk_rec_t;
typedef struct {
    int16_t  type;
    int16_t  code;
    int32_t  id;
    int32_t  x, y;
    int32_t  w, h;
} ce_blk_event_t;

/* cap2：声明记录扩展（append-only；前 40B 与 ce_blk_rec_t 完全一致）。
 * 旧应用仍按 40B 记录提交；应用以 rec2 提交时先探测能力（decl_caps），
 * 内核按 base 之后的扩展段取新字段。缺省全 0 = 旧行为。 */
typedef struct {
    ce_blk_rec_t base;      /* 40B，语义不变 */
    uint8_t  cap_ver;       /* 1 = rec2；其它由内核拒绝 */
    uint8_t  alpha;         /* 0=未声明(≡255)；1..255 */
    uint8_t  alpha_mode;    /* 0=ordered16 1=none */
    uint8_t  elevation;     /* 0..5 */
    uint8_t  corner_tl, corner_tr, corner_br, corner_bl; /* 0..63/128=pill/255=auto */
    uint8_t  wrap_lines;    /* 0/1..6/255 */
    uint8_t  ellipsis;      /* 0/1/2 */
    uint8_t  font_size;     /* 0/1/2/3/4 */
    uint8_t  font_bold;
    uint8_t  font_tabular;
    uint8_t  line_h;
    uint8_t  anim_ease;     /* 0..7 */
    uint8_t  anim_mask;     /* bit0..5 */
    uint8_t  scrollbar;     /* 0/1/2 */
    uint8_t  scroll_axis;   /* 0/1/2/3 */
    uint8_t  border_sides;  /* bit0 l..bit3 b */
    uint8_t  icon;
    uint8_t  tint_role;
    uint16_t anim_ms;
    uint16_t pop_max_h;
    uint32_t anchor_id;
    uint8_t  pop_prefer;
    uint8_t  corner_shape;
    uint8_t  pad_[2];
} ce_blk_rec2_t;
#define CE_BLK_REC2_VER 1

/* cap2：事件结构 append-only 扩展（前 20B 与 ce_blk_event_t 一致）。 */
typedef struct {
    int16_t  type;
    int16_t  code;
    int32_t  id;
    int32_t  x, y;
    int32_t  w, h;
    int32_t  target_id;
    uint8_t  ev_kind;       /* 0=click 1=focus 2=blur 3=input 4=wheel 5=key 6=dismiss */
    uint8_t  mods;
    int16_t  wheel_dx, wheel_dy;
    uint8_t  utext[24];     /* UTF-8，NUL 结尾 */
    uint32_t seq;
} ce_blk_event2_t;
#endif

/* ---- v2.81: shared result codes (kernel + probes read the same table) ----
 * Legacy negatives stay: -8 ENOSYS, -12 ENOMEM, -14 EFAULT, -16 EBUSY,
 * -22 EINVAL.  The canvas/permission codes below are stable append-only. */
#ifndef CATOS_CV_ERR_H
#define CATOS_CV_ERR_H
#define E_EPERM            (-1)   /* command drawing gated off for CE       */
#define E_CANVAS_KIND      (-30)  /* bad canvas object / kind                */
#define E_CANVAS_QUOTA     (-31)  /* per-task / size / rate quota exceeded   */
#define E_CANVAS_BOUNDS    (-32)  /* rect or dimensions out of range         */
#define E_PIX_FIELD        (-33)  /* commit record carries inline pixel field*/
#define E_CANVAS_BADH      (-34)  /* unknown / foreign resource handle       */
#endif

/* gfx_info() selectors (user-visible mirror of gfx24.h). */
#define GFX_INFO_ACTIVE    0   /* 1 = 24bpp LFB live                      */
#define GFX_INFO_WIDTH     1
#define GFX_INFO_HEIGHT    2
#define GFX_INFO_BPP       3
#define GFX_INFO_PITCH     4
#define GFX_INFO_FB_BYTES  5
#define GFX_INFO_LFB_PHYS  6
/* v2.20 append-only: physical device behind the RGB24 surface. */
#define GFX_INFO_BACKEND   7   /* 0 = Bochs dispi, 1 = VMware SVGA II     */
#define GFX_INFO_HW_BPP    8   /* 24 (Bochs) or 32 (SVGA XRGB)            */
#define GFX_INFO_STRIDE    9   /* bytes per pixel: 3 or 4                  */

typedef struct {
    uint8_t  magic[4];     /* "CE01" */
    uint32_t entry;        /* entry offset inside the image (0 = start) */
    uint32_t bss_size;     /* bytes of zeroed bss after the image */
    uint32_t api_offset;   /* offset of the 4-byte API pointer slot */
    uint32_t image_size;   /* raw image bytes following the header */
} ce_header_t;

/* directory listing callback used by catos_api_t::ls */
typedef void (*ce_ls_cb)(int is_dir, const char *name, uint32_t size, void *ud);

/* system-call table: 61 slots, v2.11 */
/* Stage 3 block 4: locale categories (mirror kernel locale.h). LC_COLLATE is
 * explicitly unsupported. */
#define LC_CTYPE_CAT     0
#define LC_NUMERIC_CAT   1
#define LC_TIME_CAT      2
#define LC_MONETARY_CAT  3
#define LC_COLLATE_CAT   4
#define LC_ALL_CAT       5
/* locale_query() selectors */
#define LQ_NAME_Q        0
#define LQ_DECIMAL_Q     1
#define LQ_THOUSANDS_Q   2
#define LQ_CURRENCY_Q    3
#define LQ_MONTH_Q       100   /* + month 1..12 */
#define LQ_WDAY_Q        200   /* + wday 0..6 */

typedef struct catos_api {
    /* ---- console ---- */
    void   (*write)(const char *s);                 /* 0  */
    void   (*putc)(char c);                         /* 1  */
    void   (*cls)(void);                            /* 2  */
    /* ---- input ---- */
    int    (*readline)(char *buf, int max);         /* 3  */
    /* ---- time ---- */
    void   (*time_str)(char *out);                  /* 4  "YYYY-MM-DD HH:MM:SS" */
    /* ---- filesystem ---- */
    int    (*fs_ready)(void);                       /* 5  */
    int    (*ls)(const char *path, ce_ls_cb cb, void *ud);  /* 6 */
    int    (*touch)(const char *path);              /* 7  */
    int    (*mkdir)(const char *path);              /* 8  */
    int    (*cat)(const char *path, char *buf, int max);    /* 9 */
    int    (*save)(const char *path, const char *data, uint32_t size); /* 10 */
    /* ---- memory ---- */
    void * (*alloc)(uint32_t size);                 /* 11 */
    void   (*free)(void *p);                        /* 12 */
    /* ---- misc ---- */
    void   (*exit)(int code);                       /* 13 */
    /* ---- beginner-friendly helpers ---- */
    void   (*print_int)(int v);                     /* 14 decimal, negatives ok */
    void   (*print_hex)(uint32_t v);                /* 15 "0x" + 8 hex digits */
    void   (*set_fg)(int color);                    /* 16 0..15 */
    void   (*delay)(uint32_t ms);                   /* 17 busy-wait */
    int    (*atoi)(const char *s);                  /* 18 decimal -> int */
    int    (*strlen)(const char *s);                /* 19 */
    int    (*strcmp)(const char *a, const char *b); /* 20 */
    void   (*strcpy)(char *d, const char *s);       /* 21 */
    int    (*getch)(void);                          /* 22 blocking key */
    int    (*file_exists)(const char *path);        /* 23 1 = exists */
    int    (*fsize)(const char *path);              /* 24 bytes, -1 absent */
    /* ---- networking (AMD PCnet-PCI, QEMU user-net) ---- */
    int    (*net_init)(void);                       /* 25 */
    int    (*net_up)(void);                         /* 26 */
    int    (*http_get)(const char *url, char *buf, int max);  /* 27 */
    int    (*net_dns)(const char *host, char *ip_out);        /* 28 */
    int    (*net_udp_send)(const char *ip, int port, const char *data, int len); /* 29 */
    int    (*net_udp_recv)(char *ip, int *port, char *buf, int max);             /* 30 */
    int    (*net_ping)(const char *ip);             /* 31 */
    void   (*net_ip)(char *out);                    /* 32 */
    /* ---- v2.3 task extensions ---- */
    int    (*getuid)(void);                         /* 33 0=root, 100=user */
    int    (*gettid)(void);                         /* 34 */
    int    (*kill)(int tid);                        /* 35 same-uid or root */
    void   (*yield)(void);                          /* 36 */
    /* ---- v2.4 hardware properties ---- */
    int    (*hw_num)(int what);                     /* 37 numeric props */
    int    (*hw_str)(int what, char *buf, int max); /* 38 string props */
    /* ---- v2.5 kernel-side: big files / pipes / processes ---- */
    int    (*file_read)(const char *path, char *buf, int max, int offset); /* 39 */
    /*   offset read; bytes copied (>=0), -1 noent, -2 EACCES, -3 bad args;
     *   max <= 16384 per call */
    int    (*file_write)(const char *path, const char *data, int size, int mode); /* 40 */
    /*   mode 0 = truncate-replace, 1 = append; size <= 16384 per call;
     *   0 = ok, negative = error */
    int    (*file_delete)(const char *path);        /* 41 0 ok, -1 noent,
                                                   *    -13 EACCES (POSIX EACCES
                                                   *    = 13; the read-only
                                                   *    built-in apps are
                                                   *    refused with this) */
    int    (*pipe_create)(void);                    /* 42 handle >= 0, -1 if full */
    int    (*pipe_write)(int h, const char *data, int len);   /* 43 blocking; 0 ok */
    int    (*pipe_read)(int h, char *buf, int max);           /* 44 blocking; bytes, 0 EOF */
    int    (*pipe_close)(int h);                    /* 45 */
    int    (*spawn)(const char *path, int argc, char **argv); /* 46 tid > 0, -err */
    int    (*wait)(int tid);                        /* 47 child exit code */
    /* ---- v2.6: memory management (append-only; old binaries see NULL) -- */
    void * (*brk)(void *addr);                      /* 48 set/get program break (0=query) */
    /* ---- v2.7: display front-end ------------------------------------
     * Mode switching + styled text ONLY.  No pixel/rect/framebuffer call
     * exists, by design.  Old binaries see NULL for these slots. */
    int    (*display_mode)(int mode);               /* 49
         mode: -1 query, 0 text, 1 graphics.
         TEXT->GFX: allowed.  GFX->TEXT: the kernel asks the human at the
         console for a physical 'Y'; a program cannot supply consent.
         returns current mode (>=0) or negative error (-13 = denied). */
    int    (*display_info)(int what);               /* 50
         0 mode, 1 cols, 2 rows, 3 colours, 4 style bitmask,
         5 pixel width, 6 pixel height; unknown -> -8 */
    void   (*set_style)(int fg, int bg, int attrs);/* 51
         fg/bg are palette indices 0..255 (text mode maps to 16);
         attrs is a CON_ATTR_* bitmask (see render.h) */
    /* ---- v2.8: truecolour + user palette (append-only) --------------- */
    void   (*set_style_rgb)(uint32_t fg, uint32_t bg, int attrs); /* 52
         fg/bg are literal 0x00RRGGBB.  On 8bpp back-ends they are
         quantised to the nearest CLUT entry; on VBE 32bpp they are used
         verbatim -- that is full 16.7M-colour text. */
    int    (*set_palette)(int idx, uint32_t rgb);   /* 53
         redefine CLUT[idx] (0..255) as 0x00RRGGBB.  Mode 13h writes the
         6-bit VGA DAC; VBE keeps it in software for indexed cells.
         Returns 0. */
    uint32_t (*palette)(int idx);                   /* 54
         read back CLUT[idx] as 0x00RRGGBB. */

    /* ---- v2.10: anonymous mmap (append-only) -------------------------- */
    void   *(*mmap)(void *addr, uint32_t len, int prot, int flags); /* 55
         addr must be NULL (the kernel picks); prot bit0 = writable;
         flags reserved.  Returns the user address after an 8 KiB guard
         page, or NULL (-22 EINVAL / -12 ENOMEM as (void*)-err).  Pages
         appear lazily on first touch. */
    int     (*munmap)(void *addr, uint32_t len);    /* 56
         unmap + free pages of a previous mmap.  0 or negative errno. */

    /* ---- v2.11: Sound Blaster 16 PCM audio (append-only) --------------
     * Old binaries see NULL for these four slots: always check
     * api->snd_info before calling them.  A machine booted without an
     * SB16 answers snd_info(SND_INFO_READY) == 0. */
    int     (*snd_info)(int what);                  /* 57
         SND_INFO_* selector; -8 ENOSYS for an unknown selector, or
         -8 from the whole driver when the card is absent. */
    int     (*snd_play)(const void *pcm, int len, int rate); /* 58
         Start an 8-bit UNSIGNED MONO DMA block.  len <= 32768,
         rate 5000..45000.  Asynchronous: poll SND_INFO_PLAYING.
         0 ok, -8 no card, -22 EINVAL, -16 EBUSY, -14 EFAULT. */
    int     (*snd_stop)(void);                      /* 59
         Halt the current 8-bit DMA transfer.  0 or -8. */
    int     (*snd_tone)(int freq_hz, int ms);       /* 60
         Kernel-generated integer sine at 22050 Hz; asynchronous.
         0 ok, negative errno. */

    /* ---- v2.12: 800x600x24 truecolour drawing (append-only) ----------
     * Old binaries see NULL for these three slots: ALWAYS check
     * api->gfx_info before calling them.  A kernel booted in text / 8bpp /
     * 32bpp mode answers gfx_info(GFX_INFO_ACTIVE) == 0.  Switch to mode 3
     * with api->display_mode(3) (or the shell's `gfx24`) first.  The LFB
     * stays supervisor-only: all writes are performed by the kernel. */
    int     (*gfx_info)(int what);                  /* 61
         GFX_INFO_* selector.  0 when inactive; -8 unknown selector. */
    int     (*gfx_rect)(const gfx_rect_t *r);      /* 62
         Fill a clipped rectangle.  0 ok, -8 inactive, -14 EFAULT for a
         bad descriptor pointer, -22 EINVAL for bad dimensions. */
    int     (*gfx_blit)(const gfx_blit_t *b);      /* 63
         Copy a tightly packed RGB24 bitmap.  The kernel reads the
         descriptor and then one row at a time through copy_from_user(),
         so a cross-page user buffer is validated page by page.
         0 ok, -8 inactive, -14 EFAULT, -22 EINVAL. */

    /* ---- v2.14: non-blocking keyboard (append-only) -------------------
     * Old binaries see NULL for this slot: ALWAYS check for NULL before
     * calling it.  Answers the classic game/animation need: read a key
     * IF one is pending, never wait.  The blocking twin is getch(). */
    int     (*pollkey)(void);                      /* 64
         Decode and return one pending key (ASCII or KB_* >0xFF),
         or -1 when nothing is pending.  Modifier presses/releases and
         break codes are consumed silently.  Never blocks, never yields. */

    /* ---- #60 (v2.19): line addressing (append-only) -------------------
     * Old binaries see NULL here.  Move the console cursor to (x, y) so a
     * TUI can rewrite one line in place -- the alternative, cls() plus a
     * full repaint, shows the blank frame in between ("flicker"). */
    int     (*gotoxy)(int x, int y);               /* 65
         0 on success, -3 if x/y is outside the screen. */

    /* ---- P1 (v2.69): generic mouse state (append-only) ----------------
     * For user-mode desktop event loops.  Old binaries see NULL here:
     * ALWAYS check for NULL before calling.  These are pure snapshots of
     * the live PS/2 pointer; no framebuffer pointer is exposed.  Slot 68
     * returns the button bitmask (1=left, 2=middle, 4=right), matching the
     * kernel MOUSE_BTN_* values. */
    int     (*mouse_x)(void);                      /* 66 absolute px */
    int     (*mouse_y)(void);                      /* 67 absolute px */
    int     (*mouse_buttons)(void);                /* 68 bitmask */

    /* ---- v2.81: isolated canvas channel (append-only) ------------------
     * The ONLY legal pixel exit for a CE desktop app.  The app draws into
     * its own extension-arena buffer (anonymous mmap, NOT VRAM, NOT a
     * compositor surface); present() copies the dirty sub-rectangle into
     * the kernel-owned backing buffer.  Old binaries see NULL: check before
     * calling.  Slots 69/70/71. */
    int     (*blk_canvas_create)(int w, int h, ce_canvas_t *out); /* 69 */
    int     (*blk_canvas_present)(const cv_present_t *r);         /* 70 */
    int     (*blk_canvas_destroy)(int res);                        /* 71 */
    /* P3 declarative channel, slots 72..76 (append-only). */
    int     (*blk_commit)(const void *recs, int n, const char *strings, int slen); /*72*/
    int     (*blk_event_poll)(ce_blk_event_t *ev, int timeout_ms);                /*73*/
    int     (*blk_image_load)(const char *path);                                  /*74*/
    int     (*blk_image_free)(int res);                                            /*75*/
    int     (*blk_text_width)(const char *s);                                     /*76*/
    /* cap2 decl channel v2, slots 77..79 (append-only). */
    uint32_t (*decl_caps)(void);                                                  /*77*/
    int     (*blk_commit2)(const void *recs, int n, const char *strings,
                           int slen, uint32_t rec_size);                          /*78*/
    int     (*blk_event_poll2)(ce_blk_event2_t *ev, int timeout_ms);              /*79*/
    /* Stage 2B: file-backed mapping slots, append-only (80..82).
     * Reserved in the ABI this stage; the file-backed path needs an fd
     * handle table the CE layer does not expose yet, so the kernel dispatch
     * returns -8 ENOSYS for now.  80 file_mmap / 81 mprotect / 82 munmap_shared. */
    uint32_t (*file_mmap)(int fd, uint32_t len, int prot, int flags);             /*80*/
    int      (*mprotect)(uint32_t addr, uint32_t len, int prot);                  /*81*/
    int      (*munmap_shared)(uint32_t addr, uint32_t len);                       /*82*/
    /* Stage 2A: CSO1 fixed-base shared object, append-only (83..85).
     * dlopen(path) -> handle (>0) or negative errno; dlsym(handle, name)
     * -> function pointer, NULL on unknown symbol / bad handle; dlclose
     * (handle) -> 0 ok or negative errno. */
    int      (*dlopen)(const char *path);                                         /*83*/
    void *   (*dlsym)(int handle, const char *name);                              /*84*/
    int      (*dlclose)(int handle);                                              /*85*/
    /* Stage 3A: user threads (append-only, slots 86..89).  A worker thread
     * shares the caller's address space (VMA/page table) but has its own
     * kernel slot, user stack and join state.  thread_create(entry, arg) ->
     * tid > 0 / negative errno; thread_exit(retval) terminates the caller;
     * thread_join(tid) blocks until the thread exits and returns its retval;
     * thread_detach(tid) makes exit auto-reap.  NOTE: this stage reserves
     * the ABI surface only; the live shared-space thread path is deferred
     * (see docs/threading.md), dispatch returns ENOSYS. */
    int      (*thread_create)(void *entry, void *arg);                            /*86*/
    void     (*thread_exit)(void *retval);                                        /*87*/
    void *   (*thread_join)(int tid);                                             /*88*/
    int      (*thread_detach)(int tid);                                          /*89*/
    /* Stage 2B: CE fd handle table (append-only, slots 90..94).  fd 0/1/2
     * are reserved stdin/stdout/stderr; user fds start at 3.  The legacy
     * path-based file_read/file_write/file_delete slots are unchanged. */
    int      (*fd_open)(const char *path, int flags);                            /*90*/
    int      (*fd_close)(int fd);                                                  /*91*/
    int      (*fd_read)(int fd, void *buf, int len);                               /*92*/
    int      (*fd_write)(int fd, const void *buf, int len);                       /*93*/
    int      (*fd_lseek)(int fd, int off, int whence);                            /*94*/
    /* Stage 2B diagnostics: what=0 resident cached pages, what=1 cumulative
     * cache hits, what=2 refcnt of the cached page covering arg (a user VA;
     * -1 if none).  Returns the requested value as an unsigned word. */
    uint32_t (*fcache_stat)(int what, uint32_t arg);                              /*95*/
    /* Stage 3 block 3: scheduler switch count for the calling task (no args). */
    uint32_t (*thread_switches)(void);                                              /*96*/
    /* Stage 3 block 4: minimal locale. setlocale(category, name) -> 0/negative;
     * locale_query(category, what, buf, len) copies a NUL-terminated locale
     * string into buf. LC_COLLATE is unsupported (setlocale returns -22). */
    int      (*setlocale)(int category, const char *name);                          /*97*/
    int      (*locale_query)(int category, int what, char *buf, int len);            /*98*/} catos_api_t;

/* snd_info() selectors.  Kept in the user-visible header so CE apps do
 * not have to include the kernel-only snd.h.  Values are mirrored there. */
#define SND_INFO_READY    0   /* 1 = SB16 probed and IRQ5 unmasked        */
#define SND_INFO_VERSION  1   /* DSP version, 0xMMmm                      */
#define SND_INFO_PLAYING  2   /* 1 = DMA block still in flight             */
#define SND_INFO_IRQS     3   /* acknowledged 8-bit DMA completion IRQs    */
#define SND_INFO_ERROR    4   /* last driver error code                    */
#define SND_INFO_BUFSZ    5   /* DMA buffer capacity (32768)               */
#define SND_INFO_RATE     6   /* sample rate of the last play request      */
#define SND_INFO_IRQSET   7   /* mixer 0x80 IRQ bitmap (diagnostic)        */
#define SND_INFO_DMASET   8   /* mixer 0x81 DMA bitmap (diagnostic)        */
/* v2.21 #56: append-only selector, no new syscall slot.  0=none,
 * 1=SB16(ISA), 2=ES1370/1371(PCI).  Old kernels return -8 (ENOSYS) and
 * callers must fall back to the IRQ-count completion check. */
#define SND_INFO_BACKEND  9

extern catos_api_t *__ce_api;

/* Fixed address of the vsyscall table (VS_TABLE).  CE binaries get the
 * pointer injected into their __ce_api slot; ELF binaries (no such slot)
 * call through this constant address:  call dword [CATOS_VS_TABLE + 4*slot] */
#define CATOS_VS_TABLE 0x2080800u

/* Call the system-call table from user code.  Declaring the slot in the
 * ".ce_api" section tells the linker where to reserve the 4-byte cell
 * that the kernel fills in before entry:
 *
 *     catos_api_t *__ce_api __attribute__((section(".ce_api")));
 *     ... api->write("hi");
 */
#ifndef CE_API
#define CE_API() (__ce_api)
#endif

#endif /* CE_ABI_H */
