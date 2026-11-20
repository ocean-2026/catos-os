/* k64.h - CatOS 64 (kernel64 P0) common definitions
 *
 * Parallel 64-bit kernel per docs/PLAN-通用操作系统.md P0:
 *   - one flat binary, GRUB multiboot AOUT, loaded at PA 0x08000000
 *   - runs higher-half at VA 0xFFFF80000000 (PML4, one 1-GiB PUD window
 *     over physical 0x00000000-0x100000000)
 *   - user 2-MiB identity pages at VA 0x7F000000 (demo ELF64 text/data)
 *     and VA 0x7F200000 (user stack) - prewired by the trampoline
 *   - 32-bit trampoline + 64-bit stub at the start of the image; the
 *     fixed physical layout of the PML4/GDT/LDT/TSS64 tables lets the
 *     32-bit trampoline reference everything with abs32 constants.
 *
 * The legacy kernel_v25 tree is untouched (I7): this is a sibling.
 */
#ifndef K64_H
#define K64_H

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef unsigned long  u64;

/* ---- fixed physical layout (must match k64.ld + boot64.s) ---------- */
#define KPHYS0      0x08000000ULL          /* GRUB flat load address     */
#define KVA0        0xFFFF80000000ULL      /* higher-half kernel VA      */
#define KDELTA      (KVA0 - KPHYS0)       /* 0xFFFF78000000             */

/* page-table tables (in .pgt, fixed offsets within the image) */
#define PML4_PA     0x08002000ULL
#define PDPT_PA     0x08003000ULL
#define PUDU0_PA    0x08004000ULL          /* user PUD, VA 0-1 GiB     */
#define PUDK_PA     0x08005000ULL          /* kernel PUD (1 GiB page)  */
#define GDT_PA      0x08006000ULL
#define LDT_PA      0x08006080ULL
#define TSS64_PA    0x080060C0ULL
#define MBSTASH     0x08009FF0ULL          /* trampoline stack-area stash */
#define TSTACK_TOP  0x08009000ULL          /* 32-bit trampoline stack  */

/* user VA plan (P0 demo) */
#define DEMO_VA         0x7F001000ULL      /* demo ELF64 entry/text    */
#define DEMO_TEXT_PU    0x7F000000ULL      /* PUD0[0x3F8] 2 MiB page  */
#define USER_STK_PU     0x7F200000ULL      /* PUD0[0x3F9] 2 MiB page  */
#define USER_STK_TOP    0x7F202000ULL      /* TSS.RSP0 / initial user RSP */
#define USER_REGION_HI  0x7F300000ULL      /* P0.6 reserved ceiling    */

#define p2v(p)  ((u64)(p) - KPHYS0 + KVA0)
#define v2p(v)  ((u64)(v) - KVA0 + KPHYS0)

/* ---- multiboot v1 (MEMINFO) ---------------------------------------- */
struct multiboot {
    u32 flags;
    u32 mem_lower;      /* KiB below 4 GiB */
    u32 mem_upper;      /* KiB above 4 GiB */
    u32 boot_device;
    u32 cmdline;
    u32 mods_count;
    u32 mods_addr;
    u32 syms_1;
    u32 num;
    u32 addr1;
    u32 long_syms;
    u32 load_addr;
    u32 load_end_addr;
    u32 reserved;
    u32 mmap_size;
    u32 mmap_addr;
};

struct mmap_entry {
    u32 size;           /* including this field */
    u64 addr;
    u64 len;
    u32 type;           /* 1 = usable */
};

/* ---- page-table symbols (defined in k64.ld, written by trampoline) - */
extern u64 pml4_t[];
extern u64 pdpt_t[];
extern u64 pudu0_t[];
extern u64 pudk_t[];
extern u64 gdt64[];
extern u64 ldt64[];

/* ---- 64-bit TSS (TSS64 layout, 104 bytes) -------------------------- */
struct tss64 {
    u32 reserved0;
    u64 rsp0, rsp1, rsp2;
    u32 reserved1;                 /* offsets 40-43 hold the LDTR sel */
    u64 ist1_top, ist1_size,
         ist2_top, ist2_size,
         ist3_top, ist3_size;
    u32 reserved2;
    u32 iopm_off;
};
extern struct tss64 tss64;

#define GDT_NULL    0x00
#define GDT_KCS64   0x10          /* idx 2: 64-bit kernel code       */
#define GDT_KDS64   0x18          /* idx 3: 64-bit kernel data/SS    */
#define GDT_LDT     0x20          /* idx 4: LDT descriptor          */
#define GDT_TSS64   0x28          /* idx 5: 64-bit TSS descriptor   */
#define GDT_UCS32   0x30          /* idx 6 (reserved, P1.9 compat)  */
#define GDT_UDS32   0x38          /* idx 7 (reserved, P1.9 compat)  */
#define LDT_UCS64   0x07          /* LDT idx 0, TI=1, RPL=3        */
#define LDT_UDS64   0x0F          /* LDT idx 1, TI=1, RPL=3        */

/* ---- pmm64 (P0.3): 1 bit per 4 KiB frame, covers 0 - 4 GiB -------- */
#define PMM_CAP     0x10000000ULL
extern u8 pmm_bitmap[];
u64  pmm_init(struct multiboot *mb, u64 kernel_image_end);
u64  pmm_alloc_page(void);
void pmm_free_page(u64 pa);
u64  pmm_total_frames(void);
u64  pmm_used_frames(void);
u64  pmm_free_frames(void);

/* ---- paging64 (P0.2) ----------------------------------------------- */
void paging64_note(void);           /* self-checks + banner line */

/* ---- serial / PIT / PIC (P0.4) -------------------------------------- */
void serial_init(void);
void serial_putc(char c);
void serial_puts(const char *s);
void serial_hex(u64 v);
void serial_dec(u64 v);
int  serial_getc(void);             /* -1 if empty */
void intr64_init(void);            /* PIC + PIT 1 kHz, IRQ0 -> counter */
u64  kuptime_ms(void);

/* ---- idt64 (P0.4) ---------------------------------------------------- */
void idt64_init(void);
void exc_a_report(u64 vec, u64 rip);
void exc_b_report(u64 vec, u64 err, u64 rip);
void exc_c_report(u64 vec, u64 cr2, u64 err, u64 rip);
void pit_irq(void);

/* ---- syscall64 (P0.5) ------------------------------------------------ */
void syscall64_init(void);
u64  sys64_dispatch(u64 snum, u64 buf_va, u64 len);
/* dispatch return: 0 = back to user via SYSRET, 1 = user exited */

/* ---- elf64 demo (P0.6) ----------------------------------------------- */
extern const u8  demo_elf[];
extern const u32 demo_elf_size;
void elf64_demo_run(void);

/* ---- kernel entry & REPL ---------------------------------------------- */
void k64_main(struct multiboot *mb_info);
void repl_resume(void);

#define HLT() do { __asm__ volatile("cli\n\t" "hlt"); } while (0)

#endif /* K64_H */
