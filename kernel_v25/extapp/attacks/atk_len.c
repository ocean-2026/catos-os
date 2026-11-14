/* atk_len.c -- attack vector: out-of-range / wrapped LENGTH arguments on
 * every length-bearing syscall, with the buffer itself valid so the length
 * is the only thing that can be wrong.
 *
 * WHY IT MIGHT CRASH
 *   Length bugs are where 32-bit arithmetic bites: `addr + len` wrapping,
 *   `max <= 0` accepted as "unlimited", and a negative int length that
 *   becomes a huge unsigned size when compared.  A ring0 copy with a
 *   wrapped length writes far outside the caller's buffer, into kernel
 *   frames belonging to somebody else (C5) or into an unmapped page (C1).
 *   Every buffer used here is a live page in this task, so a correct
 *   kernel must answer with an error code, not die.
 */
#include "atk_common.h"

catos_api_t *__ce_api __attribute__((section(".ce_api"))) = (catos_api_t *)0;

static char buf[8192];
static char big[70000];

__attribute__((section(".text.ce_entry")))
int ce_main(int argc, char **argv) {
    int i;
    catos_api_t *api = A();
    (void)argc; (void)argv;

    w("ATK-LEN: length/offset abuse with valid buffers\n");
    for (i = 0; i < 8192; i++) buf[i] = (char)('A' + (i % 26));
    for (i = 0; i < 70000; i++) big[i] = 'Z';   /* no NUL anywhere */

    /* ---- 9 cat(path, buf, max) ---- */
    step(1); mark("cat max = 0 / -1 / INT_MAX / 4097 / 1\n");
    {
        static const int L[] = { 0, -1, -4096, 0x7FFFFFFF, 0x40000000, 4097, 1, 4096 };
        for (i = 0; i < 8; i++) {
            m("max", L[i]);
            wd(api->cat("/BIN/HELLO.CE", buf, L[i]));
            w(" ");
        }
        nl();
    }

    /* ---- 10 save(path, data, uint32 size) ---- */
    step(2); mark("save size = 0 / -1 / 0xFFFFFFFF / 16385\n");
    {
        static const unsigned int S[] = { 0u, 0xFFFFFFFFu, 0x80000000u, 16385u,
                                          0x7FFFFFFFu, 65536u };
        for (i = 0; i < 6; i++) {
            mh("size", S[i]);
            wd(api->save("/TMP/ATK_LEN.TXT", buf, S[i]));
            w(" ");
        }
        nl();
    }

    /* ---- 39 file_read(path, buf, max, offset) ---- */
    step(3); mark("file_read max/offset negatives and wraps\n");
    {
        static const int L[] = { -1, 0, 0x7FFFFFFF, 16385, 16384 };
        static const int O[] = { -1, -4096, 0, 1, 0x7FFFFFFF, 0x40000000 };
        int a, b;
        for (a = 0; a < 5; a++) for (b = 0; b < 6; b++) {
            wd(api->file_read("/BIN/HELLO.CE", buf, L[a], O[b]));
            w(" ");
        }
        nl();
    }

    /* ---- 40 file_write(path, data, size, mode) ---- */
    step(4); mark("file_write size/mode abuse\n");
    {
        static const int L[] = { -1, 0x7FFFFFFF, 16385, 16384, 0 };
        static const int M[] = { -1, 2, 0x7FFFFFFF, 0, 1 };
        int a, b;
        for (a = 0; a < 5; a++) for (b = 0; b < 5; b++) {
            wd(api->file_write("/TMP/ATK_LEN.TXT", buf, L[a], M[b]));
            w(" ");
        }
        nl();
    }

    /* ---- 38 hw_str(what, buf, max) ---- */
    step(5); mark("hw_str unknown selector + max edges\n");
    {
        static const int W[] = { -1, 0, 1, 2, 3, 99, 0x7FFFFFFF };
        static const int L[] = { -1, 0, 1, 4095, 4096, 4097, 0x7FFFFFFF };
        int a, b;
        for (a = 0; a < 7; a++) for (b = 0; b < 7; b++) {
            wd(api->hw_str(W[a], buf, L[b]));
            w(" ");
        }
        nl();
    }

    /* ---- 19 strlen / 21 strcpy: no NUL for 70 KiB ---- */
    step(6); mark("strlen/strcpy on a 70 KiB unterminated buffer\n");
    m("strlen", api->strlen(big));
    api->strcpy(buf, big);              /* void: result is the byte count */
    w("strcpy done ");
    nl();

    /* ---- 0 write(): unterminated 64 KiB scan cap ---- */
    step(7); mark("write() of the unterminated 70 KiB buffer\n");
    api->write(big);            /* must stop at SYS_WRITE_MAX (64 KiB) */
    w("\n");

    /* ---- 3 readline: invalid buffer, huge max (must not block) ---- */
    step(8); mark("readline(poison, huge) -- must return -3, not block\n");
    m("rl", api->readline((char *)0xFFFFFFFFu, 0x7FFFFFFF));
    m("rl0", api->readline((char *)0xB8000u, 0));
    m("rlneg", api->readline(buf, -1));
    nl();

    /* ---- 9/24/23 path lengths ---- */
    step(9); mark("300-char path name\n");
    for (i = 0; i < 300; i++) buf[i] = 'Q';
    buf[300] = 0;
    m("cat", api->cat(buf, big, 16));
    m("touch", api->touch(buf));
    m("mkdir", api->mkdir(buf));
    m("fsize", api->fsize(buf));
    m("exists", api->file_exists(buf));
    m("del", api->file_delete(buf));
    nl();

    w("\nATK-DONE\n");
    return 0;
}
