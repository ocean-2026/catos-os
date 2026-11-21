#ifndef CE_H
#define CE_H

#include <stdint.h>

/*
 * CE - CatOS Executable format.
 *
 * A .ce file is: 20-byte header + raw image (text + rodata + data,
 * i.e. objcopy -O binary of a freestanding i386 program).  The image is
 * loaded at CE_LOAD_BASE, the bss is zeroed right after it, the kernel
 * writes its system-call table pointer into the API slot, switches to a
 * kernel-owned stack and calls the entry point.
 *
 * CE programs are written in freestanding C, compiled with
 *   gcc -m32 -ffreestanding -fno-pic -fno-stack-protector \
 *       -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -mno-3dnow -mno-80387
 * and linked with the linker script in extapp/ce.ld so that the entry
 * point ce_main() sits at image offset 0.  The entry point is called as
 *
 *     int ce_main(int argc, char **argv)
 *
 * so a program can read its command-line arguments: `run /bin/foo.ce a b`
 * gives argc=3, argv[1]="a", argv[2]="b" (argv[0] is the .ce path).
 * A legacy ce_main(void) also works: the two arguments are simply ignored.
 */

#define CE_MAGIC0 'C'
#define CE_MAGIC1 'E'
#define CE_MAGIC2 '0'
#define CE_MAGIC3 '1'

#define CE_LOAD_BASE 0x2000000u    /* 32 MiB: above the kernel image
                                      (kernel: 0x1000000 .. ~0x1480000) */
#define CE_MAX_IMAGE 0x40000u      /* 256 KiB maximum image size
                                      (raised 2026-10-28 from 64 KiB so a
                                      real cross-compiled GCC cc1/ld fits) */
#define CE_MAX_BSS   0x40000u      /* 256 KiB maximum bss.  v2.5 fix (F6.4-c):
                                      image(256K)+bss must stay below
                                      USER_VSYSCALL at 0x2080000, i.e. bss
                                      <= 512K-image, or a large .ce's bss
                                      would be clobbered by the vsyscall
                                      page mapping (bounded, too: a corrupt
                                      .ce can never zero huge areas) */

#define CE_ARGC_MAX   16           /* argv slots passed to ce_main() */

/* ---- v2.12: 24bpp drawing descriptors (mirror of ce_abi.h) -----------
 * Same guard on purpose: including both ce.h and ce_abi.h in one
 * translation unit must not redefine these typedefs. */
#ifndef CATOS_GFX_TYPES_H
#define CATOS_GFX_TYPES_H
typedef struct {
    int32_t  x, y, w, h;
    uint32_t rgb;
} gfx_rect_t;
typedef struct {
    const uint8_t *src;
    int32_t  x, y, w, h;
} gfx_blit_t;
#endif

typedef struct {
    uint8_t  magic[4];     /* "CE01" */
    uint32_t entry;        /* entry offset inside the image (0 = start) */
    uint32_t bss_size;     /* bytes of zeroed bss after the image */
    uint32_t api_offset;   /* offset of the 4-byte API pointer slot */
    uint32_t image_size;   /* raw image bytes following the header */
} ce_header_t;

/* directory listing callback used by catos_api_t::ls */
typedef void (*ce_ls_cb)(int is_dir, const char *name, uint32_t size, void *ud);

/* system-call table.  The kernel installs a pointer to this table into
 * the CE image's API slot before calling the entry point. */
typedef struct catos_api {
    /* ---- console ---- */
    void   (*write)(const char *s);
    void   (*putc)(char c);
    void   (*cls)(void);
    /* ---- input ---- */
    int    (*readline)(char *buf, int max);
    /* ---- time ---- */
    void   (*time_str)(char *out);          /* "YYYY-MM-DD HH:MM:SS" */
    /* ---- filesystem ---- */
    int    (*fs_ready)(void);
    int    (*ls)(const char *path, ce_ls_cb cb, void *ud);
    int    (*touch)(const char *path);
    int    (*mkdir)(const char *path);
    int    (*cat)(const char *path, char *buf, int max);
    int    (*save)(const char *path, const char *data, uint32_t size);
    /* ---- memory ---- */
    void * (*alloc)(uint32_t size);
    void   (*free)(void *p);
    /* ---- misc ---- */
    void   (*exit)(int code);
    /* ---- beginner-friendly helpers (all NULL-safe / bounds-checked) ---- */
    void   (*print_int)(int v);          /* decimal, handles negatives */
    void   (*print_hex)(uint32_t v);     /* "0x" + 8 lowercase hex digits */
    void   (*set_fg)(int color);         /* foreground color 0..15 */
    void   (*delay)(uint32_t ms);        /* busy-wait (CPU loop) */
    int    (*atoi)(const char *s);       /* decimal string -> int (0 if bad) */
    int    (*strlen)(const char *s);
    int    (*strcmp)(const char *a, const char *b);
    void   (*strcpy)(char *d, const char *s);
    int    (*getch)(void);               /* blocking single key: ASCII or KB_* */
    int    (*file_exists)(const char *path);  /* 1 = exists (file or dir) */
    int    (*fsize)(const char *path);        /* file size, -1 if absent */
    /* ---- networking (AMD PCnet-PCI, QEMU user-net) ---- */
    int    (*net_init)(void);                 /* init NIC (idempotent) */
    int    (*net_up)(void);                   /* 1 when NIC is running */
    int    (*http_get)(const char *url, char *buf, int max);
    /*   HTTP GET; bytes copied (>=0) or error:
     *   -1 NIC down  -2 hostname  -3 connect  -4 http status
     *   -5 response  -6 send */
    /* ---- extended networking (UDP / DNS / ping) ---- */
    int    (*net_dns)(const char *host, char *ip_out);
    /*   resolve a hostname; fills ip_out like "10.0.2.2", 0 = ok, -1 = fail */
    int    (*net_udp_send)(const char *ip, int port, const char *data, int len);
    /*   send one UDP datagram (ip may be a hostname); 0 = ok, -1 = error */
    int    (*net_udp_recv)(char *ip, int *port, char *buf, int max);
    /*   one received datagram: src ip + port + payload; bytes, 0 = none */
    int    (*net_ping)(const char *ip);       /* ICMP echo; 0 = reply, -1 = fail */
    void   (*net_ip)(char *out);              /* our address, "10.0.2.15" */
    /* ---- v2.3 task extensions (in the api table but not the classic
     * struct: mirrored here so this header matches ce_abi.h) ---- */
    int    (*getuid)(void);                   /* 0=root, 100=user */
    int    (*gettid)(void);
    int    (*kill)(int tid);
    void   (*yield)(void);
    int    (*hw_num)(int what);
    int    (*hw_str)(int what, char *buf, int max);
    /* ---- v2.5 kernel-side: big files / pipes / processes ----
     * appended at the END; old binaries indexing only the first 39
     * members keep working unchanged.  (getuid is slot 33 above.) */
    int    (*file_read)(const char *path, char *buf, int max, int offset);
    /*   big-file read with offset; bytes copied (>=0), -1 noent,
     *   -2 EACCES, -3 bad args; max <= 16384 per call */
    int    (*file_write)(const char *path, const char *data, int size, int mode);
    /*   big-file write; mode 0 = truncate-replace, 1 = append;
     *   size <= 16384 per call; 0 = ok, negative = error */
    int    (*file_delete)(const char *path);  /* 0 = ok, -1 noent, -2 EACCES */
    int    (*pipe_create)(void);              /* handle >= 0, -1 if full */
    int    (*pipe_write)(int h, const char *data, int len);
    /*   blocking: waits for buffer space; 0 = ok, -1 bad handle */
    int    (*pipe_read)(int h, char *buf, int max);
    /*   blocking: waits for data; bytes read, 0 = EOF (writer closed),
     *   -1 bad handle */
    int    (*pipe_close)(int h);
    int    (*spawn)(const char *path, int argc, char **argv);
    /*   spawn a CE as a child (inherits caller's uid); tid > 0, -err */
    int    (*wait)(int tid);                  /* wait child exit; exit code */
    /* ---- v2.6: memory management -------------------------------------- */
    void * (*brk)(void *addr);                /* set/get program break (0=query) */
    /* ---- v2.7: display front-end (append-only) ------------------------
     * Styled text and mode switching ONLY -- no drawing API is exported
     * to ring3, in any display mode. */
    int    (*display_mode)(int mode);         /* -1 query, 0 text, 1 gfx;
                                                 gfx->text needs human consent */
    int    (*display_info)(int what);         /* see CON_INFO_* in render.h */
    void   (*set_style)(int fg, int bg, int attrs);
    /* ---- v2.8: truecolour + user palette (append-only) --------------- */
    void     (*set_style_rgb)(uint32_t fg, uint32_t bg, int attrs);
    int      (*set_palette)(int idx, uint32_t rgb);
    uint32_t (*palette)(int idx);
    /* ---- v2.10: anonymous mmap in the extension arena (append-only) -- */
    void    *(*mmap)(void *addr, uint32_t len, int prot, int flags);
    int      (*munmap)(void *addr, uint32_t len);
    /* ---- v2.11: SB16 PCM audio (append-only).  Keep in lockstep with
     * ce_abi.h slots 57..60; old binaries index only the old prefix. */
    int      (*snd_info)(int what);
    int      (*snd_play)(const void *pcm, int len, int rate);
    int      (*snd_stop)(void);
    int      (*snd_tone)(int freq_hz, int ms);
    /* ---- v2.12: 24bpp truecolour drawing (append-only).  Keep in
     * lockstep with ce_abi.h slots 61..63. */
    int      (*gfx_info)(int what);
    int      (*gfx_rect)(const gfx_rect_t *r);
    int      (*gfx_blit)(const gfx_blit_t *b);
    /* ---- v2.14: non-blocking keyboard (append-only).  Keep in
     * lockstep with ce_abi.h slot 64; old binaries see NULL. */
    int      (*pollkey)(void);
    /* ---- #60 (v2.21): line addressing (append-only).  Keep in lockstep
     * with ce_abi.h slot 65 (gotoxy); old binaries see NULL.  Was missing
     * from this kernel-side mirror until v2.69 -- the API table is built
     * from SYS_COUNT so the pointer existed, but any kernel code that
     * sized/offset this struct was one slot short. */
    int      (*gotoxy)(int x, int y);
    /* ---- P1 (v2.69): generic mouse state (append-only).  Keep in
     * lockstep with ce_abi.h slots 66..68; old binaries see NULL. */
    int      (*mouse_x)(void);
    int      (*mouse_y)(void);
    int      (*mouse_buttons)(void);
} catos_api_t;

/* install the API (called once at boot) */
void ce_init(void);
/* fill out[] with "YYYY-MM-DD HH:MM:SS" (the time_str syscall body) */
void ce_time_str_fill(char *out);
/* load a CE file as a new ring3 task (implemented in task.c); argc/argv
 * are passed to ce_main().  Returns the tid (>0) or negative error. */
int  task_spawn_ce(const char *path, int argc, char **argv, int uid);
/* write the built-in CE apps and docs (compiled into the kernel image)
 * onto the FAT32 volume: the .ce files in /bin and the .TXT docs in
 * /appdevelop.  Skips files that already exist; safe to call after
 * format or at boot. */
void ce_install_builtins(void);
/* NIGHT-P4: seed /pkg/ package containers + PKGS.TXT on the volume. */
void ce_install_pkg_repo(void);

#endif
