/* sndtest.c -- CE probe for the v2.11 Sound Blaster 16 ABI (slots 57..60).
 *
 * This is the ring3 counterpart of the shell's `sndtest` command: it
 * generates an 8-bit unsigned mono PCM sine wave in its own .bss, hands
 * the USER pointer to snd_play(), and lets the kernel copy it into the
 * low-memory ISA DMA buffer with the normal VMA-backed validation.
 * That is the interesting part of the test -- a ring3 program must never
 * be able to point DMA at arbitrary physical memory.
 *
 * Verdict lines (consumed by tests/snd_regress.py):
 *   SND CE PASS                 real SB16, DMA completion IRQ observed
 *   SND CE SKIP (no SB16)       host simulator / machine without a card
 *   SND CE FAIL                 driver reported an error
 */
#include "ce_abi.h"

catos_api_t *__ce_api __attribute__((section(".ce_api"))) = (catos_api_t *)0;

#define RATE   22050
#define NSAMP  6615                 /* 300 ms at 22050 Hz */
static unsigned char wave[NSAMP];   /* .bss, inside the CE arena */

static const signed char sine32[32] = {
      0,   25,   49,   71,   90,  106,  117,  125,
    127,  125,  117,  106,   90,   71,   49,   25,
      0,  -25,  -49,  -71,  -90, -106, -117, -125,
   -127, -125, -117, -106,  -90,  -71,  -49,  -25
};

static void puts_(const char *s) { __ce_api->write(s); }
static void putdec(int v)       { __ce_api->print_int(v); }

__attribute__((section(".text.ce_entry")))
int ce_main(void) {
    catos_api_t *api = __ce_api;

    /* Append-only ABI: an older kernel has a shorter table. */
    if (!api->snd_info || !api->snd_play) {
        puts_("SND CE SKIP (kernel has no audio ABI)\n");
        return 0;
    }
    if (api->snd_info(SND_INFO_READY) != 1) {
        puts_("SND CE SKIP (no SB16)\n");
        return 0;
    }

    /* 440 Hz sine, 8-bit unsigned mono (0x80 = silence). */
    unsigned int step = (440u << 16) / RATE;
    unsigned int phase = 0;
    for (int i = 0; i < NSAMP; i++) {
        int s = (int)sine32[(phase >> 16) & 31];
        wave[i] = (unsigned char)(128 + (s * 90) / 127);
        phase += step;
    }

    int before = api->snd_info(SND_INFO_IRQS);
    int rc = api->snd_play(wave, NSAMP, RATE);
    if (rc < 0) {
        puts_("snd: play rc=");
        putdec(rc);
        puts_("\nSND CE FAIL\n");
        return 1;
    }

    /* Playback is asynchronous: poll from user context where int 0x80's
     * interrupt gate is no longer active, so IRQ5 can land.  delay() is
     * the kernel's 10 ms sleep.  Upper bound 2 s. */
    for (int i = 0; i < 200 && api->snd_info(SND_INFO_PLAYING); i++)
        api->delay(10);

    int after = api->snd_info(SND_INFO_IRQS);
    int busy  = api->snd_info(SND_INFO_PLAYING);
    puts_("snd: ce irqs=");
    putdec(after - before);
    puts_(" busy=");
    putdec(busy);
    puts_("\n");

    if (after > before && !busy) {
        puts_("SND CE PASS\n");
        return 0;
    }
    puts_("SND CE FAIL (no DMA completion IRQ)\n");
    return 1;
}
