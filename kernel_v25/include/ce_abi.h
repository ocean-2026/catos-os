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
 *
 * Total: 55 slots in v2.8 (the v2.7 header had 52).
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
typedef struct {
    uint8_t  magic[4];     /* "CE01" */
    uint32_t entry;        /* entry offset inside the image (0 = start) */
    uint32_t bss_size;     /* bytes of zeroed bss after the image */
    uint32_t api_offset;   /* offset of the 4-byte API pointer slot */
    uint32_t image_size;   /* raw image bytes following the header */
} ce_header_t;

/* directory listing callback used by catos_api_t::ls */
typedef void (*ce_ls_cb)(int is_dir, const char *name, uint32_t size, void *ud);

/* system-call table: 57 slots, v2.10 */
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
    int    (*file_delete)(const char *path);        /* 41 0 ok, -1 noent, -2 EACCES */
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
} catos_api_t;

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
