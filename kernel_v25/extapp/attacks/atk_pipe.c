/* atk_pipe.c -- attack vector: the kernel pipe FIFOs (SYS_PIPE_CREATE /
 * WRITE / READ / CLOSE, src/task.c:1511-1589).
 *
 * WHY IT MIGHT CRASH / WEDGE
 *   The pipe table is a fixed array of 8 x 4 KiB and the handles are
 *   forged with `0x100 + idx`, so any int can be passed.  Writers block
 *   (task_sleep_ticks) when the FIFO is full and readers block when it is
 *   empty: a program that creates a pipe, fills it and never reads wedges
 *   itself AND the shell that is waiting on it (task-level DoS).  This
 *   program stays strictly below the 4 KiB capacity for the write, then
 *   closes the write end and only reads to EOF, so it is self-terminating;
 *   handle arithmetic, double close and write-after-close are all fuzzed.
 */
#include "atk_common.h"

catos_api_t *__ce_api __attribute__((section(".ce_api"))) = 0;

static char buf[4200];

__attribute__((section(".text.ce_entry")))
int ce_main(int argc, char **argv) {
    catos_api_t *api = A();
    int h[12];
    int i;
    (void)argc; (void)argv;

    w("ATK-PIPE: pipe table / handle abuse\n");
    for (i = 0; i < 4200; i++) buf[i] = (char)('0' + (i % 10));

    step(1); mark("create 12 pipes (table is 8)\n");
    for (i = 0; i < 12; i++) {
        h[i] = api->pipe_create();
        m("h", h[i]); w(" ");
    }
    nl();

    step(2); mark("bogus handles on read/write/close\n");
    {
        static const int H[] = { -1, 0, 1, 0x100 - 1, 0x100 + 8, 0x100 + 99,
                                 0x7FFFFFFF, (int)0xFFFFFFFF };
        for (i = 0; i < (int)(sizeof(H) / sizeof(H[0])); i++) {
            m("w", H[i]); wd(api->pipe_write(H[i], buf, 4)); w(" ");
            m("r", H[i]); wd(api->pipe_read(H[i], buf, 4)); w(" ");
            m("c", H[i]); wd(api->pipe_close(H[i])); w(" ");
        }
        nl();
    }

    step(3); mark("length abuse on a real pipe (never exceed the capacity)\n");
    h[0] = api->pipe_create();
    m("h", h[0]);
    nl();
    if (h[0] >= 0) {
        /* NOTE: pipe_write blocks once the FIFO is full, so the writes here
         * stop at capacity-1 (4095) and are drained before the next call. */
        m("w(-1)", api->pipe_write(h[0], buf, -1));
        m("w(0)", api->pipe_write(h[0], buf, 0));
        m("w(4100)", api->pipe_write(h[0], buf, 4100));  /* > PIPE_BUF */
        m("w(4095)", api->pipe_write(h[0], buf, 4095));  /* exactly full */
        m("r(-1)", api->pipe_read(h[0], buf, -1));
        m("r(0)", api->pipe_read(h[0], buf, 0));
        m("r(4097)", api->pipe_read(h[0], buf, 4097));   /* > PIPE_BUF */
        m("r(4095)", api->pipe_read(h[0], buf, 4095));
        m("w(100)", api->pipe_write(h[0], buf, 100));
        m("r(100)", api->pipe_read(h[0], buf, 100));
        nl();
        m("close", api->pipe_close(h[0]));
        m("close2", api->pipe_close(h[0]));
        m("close3", api->pipe_close(h[0]));
        m("w-after-close", api->pipe_write(h[0], buf, 8));
        m("r-after-close", api->pipe_read(h[0], buf, 8));
        nl();
    }

    step(4); mark("read() with poison buffers on a live pipe\n");
    h[1] = api->pipe_create();
    if (h[1] >= 0) {
        api->pipe_write(h[1], buf, 32);
        m("r@0", api->pipe_read(h[1], (char *)0u, 16));
        m("r@vram", api->pipe_read(h[1], (char *)0xB8000u, 16));
        m("r@intmax", api->pipe_read(h[1], (char *)0xFFFFFFFFu, 16));
        nl();
        /* close the write end, then drain so nothing can block */
        api->pipe_close(h[1]);
    }

    step(5); mark("close all, then channel storm (create/close x50)\n");
    for (i = 0; i < 12; i++) if (h[i] >= 0) api->pipe_close(h[i]);
    for (i = 0; i < 50; i++) {
        int x = api->pipe_create();
        if (x >= 0) api->pipe_close(x);
    }
    w("storm ok\n");

    w("\nATK-DONE\n");
    return 0;
}
