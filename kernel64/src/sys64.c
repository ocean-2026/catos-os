/* sys64.c - P0.5: syscall table behind int 0x80 (DPL3 gate)
 *   1 = write(buf,len)  2 = exit()
 */
#include "k64.h"

static int in_user_win(u64 va)
{
    return (va >= USER_CODE_VA && va < USER_CODE_VA + USER_WIN_SIZE) ||
           (va >= USER_STACK_VA && va < USER_STACK_VA + USER_WIN_SIZE);
}

u64 sys_dispatch(u64 snum, const char *buf, u64 len)
{
    switch (snum) {
    case 1: {                                  /* write */
        if (!in_user_win((u64)buf) || len > 256) {
            serial_puts("[k64] sys_write: bad range\n");
            return 0;
        }
        serial_puts("[ring3] ");
        for (u64 i = 0; i < len; i++) serial_putc(buf[i]);
        return 0;
    }
    case 2:                                    /* exit */
        serial_puts("[k64] ring3 demo exited\n");
        return 1;
    default:
        serial_puts("[k64] unknown syscall ");
        serial_dec(snum);
        serial_puts("\n");
        return 0;
    }
}

void syscall64_init(void)
{
    /* IA32_STAR: kernel CS in 47:32 (SYSCALL loads CS/SS from it) */
    u64 star = (0x20ULL << 48) | ((u64)SEL_KCS64 << 32);
    u32 lo = (u32)star, hi = (u32)(star >> 32);

    __asm__ volatile ("wrmsr" : : "c"(0xC0000081u), "a"(lo), "d"(hi));  /* STAR */
    /* LSTAR is set by boot32.s to int80_entry's 64-bit entry; re-set here
     * to make the intent explicit and to survive future relinks */
    extern void int80_entry(void);
extern void syscall_entry(void);
    u64 lstar = (u64)syscall_entry;
    __asm__ volatile ("wrmsr" : : "c"(0xC0000082u), "a"((u32)lstar), "d"((u32)(lstar >> 32)));
    __asm__ volatile ("wrmsr" : : "c"(0xC0000084u), "a"(0u), "d"(0u));  /* SFMASK */

    /* EFER.SCE */
    u32 e_lo, e_hi;
    __asm__ volatile ("rdmsr" : "=a"(e_lo), "=d"(e_hi) : "c"(0xC0000080u));
    e_lo |= 1u;
    __asm__ volatile ("wrmsr" : : "c"(0xC0000080u), "a"(e_lo), "d"(e_hi));
}



