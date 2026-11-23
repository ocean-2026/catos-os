/* k64.h - CatOS64 P0 common definitions
 *
 * P0.1 long-mode boot  : boot/boot32.s + boot/isr64.s
 * P0.2 paging          : 4-level tables, identity 0-1 GiB (2 MiB pages) +
 *                        user window VA 0x40000000/0x40200000
 * P0.3 pmm64           : 4 KiB frame bitmap for 0-4 GiB from multiboot mmap
 * P0.4 idt64/serial    : 32 exception stubs + PIC/PIT 1 kHz + COM1 printk
 * P0.5 syscall64       : int 0x80 gate (DPL3) + IA32_LSTAR/SFMASK setup
 * P0.6 elf64           : PT_LOAD loader -> ring3 demo ELF64 hello
 */
#ifndef K64_H
#define K64_H

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef unsigned long  u64;
typedef long           i64;

#define KPHYS0        0x08000000ULL     /* GRUB load address (identity VA) */
#define USER_CODE_PA  0x3E000000ULL     /* 2 MiB frame backing user text   */
#define USER_STACK_PA 0x3F200000ULL     /* 2 MiB frame backing user stack  */
#define USER_CODE_VA  0x40000000ULL     /* PDPT[1] PD[0]                   */
#define USER_STACK_VA 0x40200000ULL     /* PDPT[1] PD[1]                   */
#define USER_STACK_TOP 0x40400000ULL
#define USER_WIN_SIZE 0x400000ULL

/* GDT selectors (see boot/boot32.s) */
#define SEL_KCS64 0x08
#define SEL_KDS64 0x10
#define SEL_UCS64 0x33                  /* GDT 0x30 | RPL3, SYSRET X+16 */
#define SEL_UDS64 0x2B                  /* GDT 0x28 | RPL3, SYSRET X+8   */
#define SEL_TSS64 0x18

#define PMM_CAP   0x100000000ULL        /* 4 GiB                         */
#define PMM_FRAME 4096ULL

struct multiboot {
    u32 flags;
    u32 mem_lower;
    u32 mem_upper;
    u32 boot_device;
    u32 cmdline;
    u32 mods_count;
    u32 mods_addr;
    u32 syms[4];
    u32 mmap_length;
    u32 mmap_addr;
};

struct mmap_entry {
    u32 size;
    u64 addr;
    u64 len;
    u32 type;
} __attribute__((packed));

/* ---- serial (P0.4) -------------------------------------------------- */
void serial_init(void);
void serial_putc(char c);
void serial_puts(const char *s);
void serial_hex(u64 v);
void serial_dec(u64 v);
char serial_getc(void);                 /* 0 = empty */

/* ---- pmm64 (P0.3) --------------------------------------------------- */
void pmm_init(struct multiboot *mb, u64 kernel_end);
u64  pmm_alloc(void);
void pmm_free(u64 pa);
u64  pmm_total(void);
u64  pmm_used(void);
 u64  pmm_alloc_2m(void);   /* 2 MiB-aligned run, for user windows */

/* ---- idt64 (P0.4) ---------------------------------------------------- */
void idt64_init(void);
void pic_init(void);
void pit_init(u64 hz);
u64  kuptime_ticks(void);
void exc_dispatch(u64 vec, u64 err, u64 rip);
void irq_dispatch(u64 vec);

/* ---- syscall64 (P0.5) ------------------------------------------------ */
void syscall64_init(void);
u64  sys_dispatch(u64 snum, const char *buf, u64 len);

/* ---- elf64 demo (P0.6) ----------------------------------------------- */
extern const u8  demo_elf[];
extern const u32 demo_elf_size;
void user_window_init(void);   /* PMM-backed 2 MiB frames for VA 0x40000000/0x40200000 */
 extern u64 user_code_pa;
 extern u64 pd_user_t[512];
 void elf64_demo_run(void);

/* ---- kernel entry / REPL --------------------------------------------- */
void kmain64(struct multiboot *mb);
void repl_resume(void) __attribute__((noreturn));

extern u64 kernel_rsp_save;
extern u64 tss64[16];

#define HLT() __asm__ volatile ("cli\n\thlt")

#endif /* K64_H */



