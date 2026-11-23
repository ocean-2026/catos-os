/* e3probe.c -- E3 regression probe: per-task syscall staging buffers.
 *
 * Background (see docs/全局待办总表.md E3): several syscalls that can block
 * mid-call (SYS_WRITE, SYS_READLINE, SYS_FILE_READ, SYS_FILE_WRITE,
 * SYS_PIPE_WRITE, SYS_PIPE_READ, SYS_NET_UDP_RECV) used function-level
 * static kernel buffers.  Once a blocking call switches tasks, another
 * task's snapshot clobbers the first one's bytes.  This probe exercises
 * the concurrent pipe write/read path and the poison-pointer path.
 *
 * Modes (argv[1]):
 *   parent  : spawn one writer on a pipe, drain 8 KiB and verify every
 *             chunk matches the known cycle byte-for-byte, with interleaved
 *             console traffic to exercise the SYS_WRITE staging buffer while
 *             the writer blocks on the full pipe.
 *   writer  : child helper: pipe_write a repeating cycle, chunk 256 B.
 *   badptr  : hand kernel-space / unmapped pointers to file_write /
 *             pipe_write (and write).  Every call must return an error and
 *             the kernel must keep running (no ring0 fault / halt banner).
 *
 * Verdict lines consumed by the E3 driver:
 *     E3PROBE CONC PASS / E3PROBE CONC FAIL
 *     E3PROBE BADPTR PASS / E3PROBE BADPTR FAIL
 *
 * Built with -std=c99 -mno-80387, no libc.  Keep comments plain ASCII.
 */
#include "ce_abi.h"

catos_api_t *__ce_api __attribute__((section(".ce_api"))) = (catos_api_t *)0;

#define CHUNK 256
#define EXPECT 8192

/* The writer's 16-byte cycle.  Every drained chunk must match this cycle
 * exactly; a hybrid / garbage chunk means two snapshots collided on a
 * shared buffer. */
static const char P1[16] = "0123456789ABCDEF";

static void puts_(const char *s) { __ce_api->write(s); }
static void put_int(int v) { __ce_api->print_int(v); }

static void uitoa(int v, char *dst) {
    char tmp[12]; int k = 0;
    if (v == 0) { dst[0] = '0'; dst[1] = 0; return; }
    while (v > 0) { tmp[k++] = (char)('0' + v % 10); v /= 10; }
    int j = 0;
    while (k > 0) dst[j++] = tmp[--k];
    dst[j] = 0;
}

static void put_hex8(unsigned char v) {
    const char *h = "0123456789ABCDEF";
    char b[3]; b[0] = h[(v >> 4) & 15]; b[1] = h[v & 15]; b[2] = 0;
    __ce_api->write(b); __ce_api->write(" ");
}

static int cycle_match_at(const char *buf, int n, const char *p, int pos) {
    for (int i = 0; i < n; i++)
        if (buf[i] != p[(pos + i) & 15]) return 0;
    return 1;
}

/* ---- child writer ------------------------------------------------------- */
static int run_writer(int h, int total) {
    char chunk[CHUNK];
    for (int i = 0; i < CHUNK; i++) chunk[i] = P1[i & 15];
    int off = 0;
    while (off < total) {
        int n = total - off;
        if (n > CHUNK) n = CHUNK;
        if (__ce_api->pipe_write(h, chunk, n) != 0) return 0;
        off += n;
    }
    return 1;
}

/* ---- parent: concurrent writer drain ------------------------------------ */
static int run_parent(void) {
    char rbuf[CHUNK];
    char av[5][32];
    char *argv[5];
    char hstr[12];
    int h = __ce_api->pipe_create();
    if (h < 0) { puts_("E3PROBE CONC FAIL (pipe_create)\n"); return 1; }
    uitoa(h, hstr);

    for (int i = 0; i < 5; i++) argv[i] = av[i];
    __ce_api->strcpy(av[0], "/bin/e3probe.ce");
    __ce_api->strcpy(av[1], "writer");
    __ce_api->strcpy(av[2], hstr);
    __ce_api->strcpy(av[3], "8192");
    av[4][0] = 0;

    int tid = __ce_api->spawn("/bin/e3probe.ce", 4, argv);
    if (tid <= 0) { puts_("E3PROBE CONC FAIL (spawn)\n"); return 1; }

    int got = 0, bad = 0, chunks = 0, dumped = 0, pos = 0;
    while (got < EXPECT) {
        int n = __ce_api->pipe_read(h, rbuf, CHUNK);
        if (n <= 0) break;
        chunks++;
        if (!cycle_match_at(rbuf, n, P1, pos)) {
            bad++;
            if (dumped < 3) {
                dumped++;
                puts_("BAD#"); put_int(dumped); puts_(" n="); put_int(n); puts_(": ");
                for (int i = 0; i < 16 && i < n; i++) put_hex8((unsigned char)rbuf[i]);
                puts_("\n");
            }
        }
        pos += n;
        got += n;
        /* interleave console traffic while the writer blocks on the full
         * pipe: exercise the SYS_WRITE staging buffer on the parent side. */
        if ((chunks & 7) == 0) __ce_api->write("E3PROBE busy\n");
    }
    __ce_api->wait(tid);
    __ce_api->pipe_close(h);

    puts_("E3PROBE CONC got="); put_int(got);
    puts_(" bad="); put_int(bad);
    puts_(" expect="); put_int(EXPECT);
    if (got == EXPECT && bad == 0) { puts_(" PASS\n"); return 0; }
    puts_(" FAIL\n");
    return 1;
}

/* ---- poison pointers ---------------------------------------------------- */
static int run_badptr(void) {
    int h = __ce_api->pipe_create();
    /* kernel-ish low address and an unmapped high-ish address.  Neither is
     * inside this task's VMA list, so every validation must reject them. */
    char *kaddr = (char *)0x00100000u;
    char *unmapped = (char *)0x00FFFFF0u;
    int r1 = __ce_api->file_write("/tmp/e3bad.dat", kaddr, 16, 0);
    int r2 = __ce_api->file_write("/tmp/e3bad.dat", unmapped, 16, 0);
    int r3 = (h >= 0) ? __ce_api->pipe_write(h, kaddr, 16) : -1;
    int r4 = (h >= 0) ? __ce_api->pipe_write(h, unmapped, 16) : -1;
    /* write() returns void in the ABI; the kernel still validates the buffer
     * before reading, so a bad pointer must not fault.  Survival is the
     * assertion here. */
    __ce_api->write(kaddr);
    __ce_api->write(unmapped);
    if (h >= 0) __ce_api->pipe_close(h);

    puts_("E3PROBE BADPTR fw="); put_int(r1);
    puts_(" fw2="); put_int(r2);
    puts_(" pw="); put_int(r3);
    puts_(" pw2="); put_int(r4);
    /* file_write/pipe_write return 0 on success, negative on error. */
    if (r1 < 0 && r2 < 0 && r3 < 0 && r4 < 0) { puts_(" PASS\n"); return 0; }
    puts_(" FAIL\n");
    return 1;
}

__attribute__((section(".text.ce_entry")))
int ce_main(int argc, char **argv) {
    const char *mode = (argc >= 2 && argv[1]) ? argv[1] : "parent";
    if (__ce_api->strcmp(mode, "writer") == 0) {
        int wh = __ce_api->atoi(argv[2]);
        int total = __ce_api->atoi(argv[3]);
        return run_writer(wh, total) ? 0 : 1;
    }
    if (__ce_api->strcmp(mode, "badptr") == 0) return run_badptr();
    return run_parent();
}
