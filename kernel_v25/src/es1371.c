/* es1371.c -- Ensoniq ES1370 / ES1371 PCI audio driver for CatOS.
 *
 * =====================================================================
 * Verified against QEMU v8.2.0 hw/audio/es1370.c (the file the task told
 * us to cross-check, /d/_qemu_es1370.c).  Several points DIFFER from the
 * "generic ES1370 lore" and were taken straight from that source:
 *
 *  (1) BAR0 is I/O space, 256 bytes: es1370_realize() does
 *        memory_region_init_io(&s->io, ..., 256);
 *        pci_register_bar(&s->dev, 0, PCI_BASE_ADDRESS_SPACE_IO, &s->io);
 *      (lines 852-853).  So every register is inl/outl(io_base + off)
 *      with off in [0,255].  There is NO memory BAR to map; we deliberately
 *      accept ONLY an I/O BAR (bar0 & 1).
 *
 *  (2) MEMPAGE PAGED WINDOW (es1370_fixup, lines 452-459):
 *        addr &= 0xff;
 *        if (addr >= 0x30 && addr <= 0x3f) addr |= s->mempage << 8;
 *      The "0xc30 / 0xc34" frame registers are LOGICAL addresses.  They are
 *      reached through ports 0x30 / 0x34 after writing MEMPAGE (0x0c) = 0xc,
 *      which turns 0x34 -> 0xc34 and hits case ES1370_REG_DAC1_FRAMECNT.
 *      A direct inl(io_base + 0xc34) would fall OUTSIDE the 256-byte BAR and
 *      never reach es1370_read().  Registers < 0x30 (CONTROL, STATUS,
 *      SERIAL_CONTROL, *_SCOUNT) are NOT paged and need no MEMPAGE setup.
 *
 *  (3) FRAMECNT half-word order (es1370_transfer_audio, lines 613-618):
 *        int cnt  = d->frame_cnt >> 16;    (HIGH 16 = current progress)
 *        int size = d->frame_cnt & 0xffff; (LOW  16 = total frame count)
 *        int left = ((size - cnt + 1) << 2) + d->leftover;
 *      i.e. the LOW half is the size and the HIGH half is the running
 *      counter -- the OPPOSITE of the "high = size" note in the task brief.
 *      We write (frames-1): low=size, high=cnt=0, so left == frames*4 bytes.
 *      "<size - cnt + 1>" is why the size field is frames-1, not frames.
 *
 *  (4) DMA FORMAT (es1370_update_voices, lines 394-410; transfer_audio
 *      lines 604-694): P1FMT = 3 (SCTRL_P1SEB|SCTRL_P1SMB, 16-bit stereo)
 *      gives d->shift = (3&1)+(3>>1) = 2, i.e. a 4-byte frame of two
 *      little-endian int16_t samples.  as.fmt = AUDIO_FORMAT_S16,
 *      as.nchannels = 2, as.endianness = 0.  addr += (cnt << 2) and
 *      left = (...) << 2 are both in those 4-byte frames.  That is exactly
 *      why the 8-bit unsigned mono input must be widened and duplicated.
 *
 *  (5) INTERRUPT ACK: es1370_write() (lines 461-528) has NO case for
 *      ES1370_REG_STATUS (0x04) -- writing STATUS is a NO-OP on QEMU (it
 *      falls to default: and only lwarn()s).  Real ES1370/ES1371 and the
 *      Linux driver are write-1-to-clear on STATUS, but QEMU does not model
 *      it.  QEMU lowers a STAT_DACx bit / the PCI IRQ line ONLY in
 *      es1370_maybe_lower_irq() (lines 343-362), i.e. when the matching
 *      PnINTEN bit is CLEARED in SERIAL_CONTROL.  Worse, if PnINTEN is left
 *      set, looped DMA keeps re-asserting the IRQ -> storm.  Therefore this
 *      driver NEVER enables P1INTEN/P2INTEN; completion is decided purely
 *      in software.  es1371_irq() still does the real-hardware STATUS ack
 *      AND clears the INTEN bits (for QEMU), so it is safe on both.
 *
 *  (6) QEMU implements ONLY looped DMA: the file header lists
 *      "non looped mode" under Missing stuff (lines 39-47) and
 *      es1370_transfer_audio() resets frame_cnt back to `size` and wraps
 *      (lines 672-687).  The buffer is replayed forever; the hardware will
 *      never stop by itself.  So es1371_playing() must halt the channel
 *      (clear CTRL_DAC1_EN) when its software deadline expires.
 *
 *  (7) VOICE ACTIVATION ORDER (es1370_update_voices, lines 385-450):
 *      AUD_open_out() rebuilds the voice when fmt/freq CHANGES, while
 *      AUD_set_active_out() is called when the ctrl EN bit CHANGES.  Reset
 *      leaves ctl=1 / sctl=0 (8-bit mono voice).  We must write
 *      SERIAL_CONTROL (P1FMT=3) first to build the 16-bit stereo voice,
 *      and raise CTRL_DAC1_EN LAST to produce a 0->1 edge that activates
 *      that 16S voice.  Otherwise QEMU stays on the 8-bit mono voice and
 *      no audio is produced.  arm_dma() clears EN then sets it on every
 *      play so the edge is guaranteed.
 *
 *  (8) DAC1 rate is one of {5512, 11025, 22050, 44100} selected by
 *      CTRL_WTSRSEL (lines 80, 92, 364-370): no programmable divider on
 *      DAC1.  es1371_play() quantizes 'rate' to the nearest entry.
 *
 *  (9) PCI command: net.c already established that QEMU >= 11 gates DMA
 *      behind PCI_COMMAND_MASTER (pci_dma_read returns zeroes otherwise).
 *      es1370_transfer_audio() uses pci_dma_read(), so probe() sets
 *      IO | MEM | MASTER in the command register, copied from net.c.
 *
 * ---------------------------------------------------------------------
 * Why 0x550000 for the DMA buffer:
 *   Low 64 MiB is identity-mapped, so a physical address is a usable C
 *   pointer (same trick net.c / snd.c use).  Occupied static windows are
 *   0x500000..0x510000 (PCnet rings), 0x520000 (SB16 32 KiB), 0x530000+
 *   (reserved for the e1000 driver).  0x550000 is 64 KiB aligned and a
 *   64 KiB buffer (0x550000..0x560000) never crosses a 64 KiB boundary,
 *   which is the safest shape for a PCI DMA target.
 *
 * Why no hlt / no waiting on timer_ticks:
 *   int 0x80 is an interrupt gate (IF=0).  We only READ the intr.c global
 *   timer_ticks (100 Hz PIT) to compute elapsed time; we never spin waiting
 *   for it to change.  es1371_play() arms DMA and returns immediately;
 *   the caller polls es1371_playing()/ES1371_INFO_PLAYING from a context
 *   where interrupts are live, and each poll advances the software state
 *   machine (bounded timeout).  This is the "software completion flag +
 *   bounded timeout" fallback requested in the brief.
 * =====================================================================
 */

#include "es1371.h"
#include "io.h"

/* ---- PCI configuration space (copied verbatim from net.c) ------------ */
static uint32_t pci_cfg_read(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) |
                    ((uint32_t)(dev & 0x1F) << 11) | ((uint32_t)(fn & 7) << 8) |
                    (uint32_t)(off & 0xFC);
    outl(0xCF8, addr);
    return inl(0xCFC);
}
static void pci_cfg_write(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t v) {
    uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) |
                    ((uint32_t)(dev & 0x1F) << 11) | ((uint32_t)(fn & 7) << 8) |
                    (uint32_t)(off & 0xFC);
    outl(0xCF8, addr);
    outl(0xCFC, v);
}

/* ---- PCI ids / command bits ------------------------------------------ */
#define PCI_VENDOR_ENSONIQ  0x1274u
#define PCI_DEV_ES1370      0x5000u   /* QEMU -device es1370             */
#define PCI_DEV_ES1371      0x1371u   /* VMware / real AudioPCI ES1371   */
#define PCI_COMMAND_IO      0x0001u
#define PCI_COMMAND_MEM     0x0002u
#define PCI_COMMAND_MASTER  0x0004u   /* required for QEMU pci_dma_read  */

/* ---- Register offsets: LOW 8 bits of the 256-byte I/O BAR ------------
 * 0x30..0x3f is the MEMPAGE window; those two are reached as 0x30/0x34
 * with MEMPAGE=0xc (logical 0xc30/0xc34).  See note (2) above. */
#define R_CONTROL           0x00
#define R_STATUS            0x04
#define R_MEMPAGE           0x0c
#define R_SERIAL_CONTROL    0x20
#define R_DAC1_SCOUNT       0x24
#define R_DAC1_FRAMEADR     0x30   /* window offset -> logical 0xc30 */
#define R_DAC1_FRAMECNT     0x34   /* window offset -> logical 0xc34 */
#define MEMPAGE_DAC1        0x0c   /* page 0xc00 for 0x30..0x3f      */

/* ---- CONTROL bits (QEMU es1370.c lines 85-105) ----------------------- */
#define CTRL_DAC1_EN        0x00000040u
#define CTRL_WTSRSEL        0x00003000u
#define CTRL_SH_WTSRSEL     12
#define CTRL_XCTL0          0x00000100u   /* 1 = Line out              */

/* ---- STATUS bits (lines 107-117) -------------------------------------- */
#define STAT_DAC1           0x00000004u
#define STAT_DAC2           0x00000002u

/* ---- SERIAL_CONTROL bits, DAC1 / "P1" channel (lines 130-155) --------
 * P1FMT = P1SEB | P1SMB = 3 -> 16-bit stereo (4-byte frames).
 * P1LOOPSEL (0x2000) and P1PAUSE (0x800) are deliberately LEFT CLEAR:
 * 0 selects loop mode, the only mode QEMU implements.  P1INTEN/P2INTEN
 * are deliberately LEFT CLEAR too (see note (5): QEMU IRQ storm). */
#define SCTRL_P1SEB         0x00000002u   /* 1 = 16-bit samples         */
#define SCTRL_P1SMB         0x00000001u   /* 1 = stereo                 */
#define SCTRL_P1SCTRLD      0x00000080u   /* reload sample count        */
#define SCTRL_P1INTEN       0x00000100u
#define SCTRL_P2INTEN       0x00000200u
#define SCTRL_P1FMT         (SCTRL_P1SEB | SCTRL_P1SMB)
#define SCTRL_DAC1_BASE     (SCTRL_P1FMT | SCTRL_P1SCTRLD)

/* ---- DMA buffer geometry --------------------------------------------- */
#define DMA_PHYS            ES1371_DMA_PHYS
#define DMA_BYTES           ES1371_DMA_BUF_SZ
/* one 8-bit mono input sample -> one 4-byte (16-bit stereo) frame */
#define MAX_SAMPLES         (DMA_BYTES / 4u)

/* Identity-mapped low memory: the physical DMA address IS a C pointer.
 * volatile because the PCI bus-master reads it asynchronously. */
static volatile int16_t *const dma_buf = (volatile int16_t *)DMA_PHYS;

/* 8-bit staging area for es1371_tone(): es1371_play() reads 8-bit PCM and
 * writes 16-bit stereo into dma_buf, so the tone generator cannot reuse
 * dma_buf as its input (read/write would alias).  Lives in .bss. */
static uint8_t tone_scratch[MAX_SAMPLES];

/* ---- Time source: intr.c's 100 Hz tick counter (READ ONLY) -----------
 * We only ever take a difference; never spin waiting for it to advance. */
extern volatile uint32_t timer_ticks;

/* ---- Driver state ----------------------------------------------------- */
static int      es_probed;       /* 1 = a supported card was found       */
static int      es_ready;        /* 1 = init ok, can play                */
static int      es_busy;         /* 1 = DMA / software playback active   */
static uint16_t es_io;           /* BAR0 I/O base                        */
static uint8_t  es_irq_line;     /* PCI 0x3C low byte                    */
static uint16_t es_vendor, es_device;
static uint32_t es_irq_count;    /* acknowledged IRQs (diagnostic)       */
static int      es_last_err;     /* last error code, 0 = none            */
static uint32_t es_rate;         /* actual (quantized) sample rate       */
static uint32_t es_start_tick;   /* timer_ticks when play() armed DMA    */
static uint32_t es_play_ticks;   /* bounded software timeout, in ticks    */

/* DAC1 WTSRSEL table (QEMU dac1_samplerate[], line 80) */
static const uint32_t dac1_rates[4] = { 5512u, 11025u, 22050u, 44100u };

/* 32 samples of a full sine period, amplitude +/-127.  Integer-only
 * (kernel is built -mno-80387); copied from snd.c::snd_tone(). */
static const int8_t sine32[32] = {
      0,   25,   49,   71,   90,  106,  117,  125,
    127,  125,  117,  106,   90,   71,   49,   25,
      0,  -25,  -49,  -71,  -90, -106, -117, -125,
   -127, -125, -117, -106,  -90,  -71,  -49,  -25
};

/* ---- Low-level register helpers -------------------------------------- */
static inline uint32_t rr(uint8_t off) {
    return inl((uint16_t)(es_io + (uint16_t)off));
}
static inline void wr(uint8_t off, uint32_t v) {
    outl((uint16_t)(es_io + (uint16_t)off), v);
}
/* Select the memory page that exposes the DAC1 frame registers.  The
 * FRAMEADR/FRAMECNT ports are inside the 0x30..0x3f window, so this must
 * be issued before touching them (es1370_fixup). */
static void set_dac1_page(void) { wr(R_MEMPAGE, MEMPAGE_DAC1); }

/* Nearest WTSRSEL index (0..3) for a requested rate.  Integer-only, no
 * libm / abs().  Returns the selector, not the rate. */
static uint32_t quantize_rate(uint32_t rate) {
    uint32_t best = 0, bestd = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < 4; i++) {
        uint32_t d = (rate > dac1_rates[i]) ? (rate - dac1_rates[i])
                                            : (dac1_rates[i] - rate);
        if (d < bestd) { bestd = d; best = i; }
    }
    return best;
}

/* Clear CTRL_DAC1_EN: stops the DMA channel (and, on QEMU, calls
 * AUD_set_active_out(voice, 0)).  Used by stop()/timeout. */
static void halt_dma(void) {
    if (!es_probed) return;
    uint32_t ctl = rr(R_CONTROL);
    wr(R_CONTROL, ctl & ~CTRL_DAC1_EN);
}

/* Program the DAC1 frame registers and start the transfer.  Order matters
 * -- see note (7) about voice activation and note (2) about MEMPAGE. */
static void arm_dma(uint32_t frames, uint32_t sel) {
    uint32_t ctl_on  = CTRL_DAC1_EN | (sel << CTRL_SH_WTSRSEL) | CTRL_XCTL0;
    uint32_t ctl_off = ctl_on & ~CTRL_DAC1_EN;
    uint32_t sctl    = SCTRL_DAC1_BASE;   /* 16S, loop, no INTEN, no PAUSE */

    /* 1) Guarantee a clean DAC1_EN 0->1 edge even across back-to-back
     *    plays (update_voices only activates on the EN bit change). */
    wr(R_CONTROL, ctl_off);

    /* 2) Build the 16-bit stereo voice BEFORE raising EN. */
    wr(R_SERIAL_CONTROL, sctl);

    /* 3) Paged window: MEMPAGE=0xc maps 0x30/0x34 -> 0xc30/0xc34.
     *    FRAMECNT low half = frames-1 (size), high half = 0 (cnt).
     *    SCOUNT low half = frames-1 -> one period covers the whole block. */
    set_dac1_page();
    wr(R_DAC1_FRAMEADR, (uint32_t)DMA_PHYS);
    wr(R_DAC1_FRAMECNT, frames - 1u);
    wr(R_DAC1_SCOUNT,   frames - 1u);

    /* 4) Raise EN LAST -> activates the 16S voice and starts DMA. */
    wr(R_CONTROL, ctl_on);
}

/* ---- Public API ------------------------------------------------------- */

int es1371_probe(void) {
    if (es_probed) return 1;   /* idempotent: no repeated config writes */

    for (uint8_t dev = 0; dev < 32; dev++) {
        for (uint8_t fn = 0; fn < 8; fn++) {
            uint32_t id = pci_cfg_read(0, dev, fn, 0);
            uint16_t v  = (uint16_t)(id & 0xFFFFu);
            if (v == 0xFFFFu || v == 0) continue;          /* empty slot  */
            uint16_t d  = (uint16_t)(id >> 16);
            if (v != PCI_VENDOR_ENSONIQ) continue;
            if (d != PCI_DEV_ES1370 && d != PCI_DEV_ES1371) continue;

            uint32_t bar0 = pci_cfg_read(0, dev, fn, 0x10);
            if (!(bar0 & 1u)) continue;   /* I/O BAR only, never MEM BAR */

            es_io = (uint16_t)(bar0 & 0xFFFCu);   /* drop type bits (net.c) */
            es_irq_line = (uint8_t)(pci_cfg_read(0, dev, fn, 0x3C) & 0xFFu);
            es_vendor = v;
            es_device = d;

            /* IO + MEM + MASTER.  MASTER is essential: QEMU gates
             * pci_dma_read() behind it (net.c comment + note (9)). */
            uint16_t cmd = (uint16_t)pci_cfg_read(0, dev, fn, 0x04);
            cmd |= PCI_COMMAND_IO | PCI_COMMAND_MEM | PCI_COMMAND_MASTER;
            pci_cfg_write(0, dev, fn, 0x04, cmd);

            es_probed = 1;
            return 1;
        }
    }
    return 0;   /* no ES1370/ES1371: NOT fatal, boot continues silently */
}

int es1371_init(void) {
    es_ready = 0;
    es_busy  = 0;
    es_last_err = 0;

    if (!es1371_probe()) { es_last_err = 1; return -1; }   /* no card */

    /* QEMU reset leaves ctl=1, status=0x60, sctl=0 (an 8-bit mono voice).
     * Establish the 16-bit stereo voice and a clean CONTROL value, but
     * keep DAC1_EN clear until the first play(). */
    wr(R_SERIAL_CONTROL, SCTRL_DAC1_BASE);
    wr(R_CONTROL, CTRL_XCTL0);         /* WTSRSEL=0, DAC1_EN=0 */

    /* Clear stale interrupt bits.  Real hardware: STATUS is
     * write-1-to-clear.  QEMU: STATUS writes are a no-op (note (5)), so
     * this is harmless there; es1371_irq() also clears PnINTEN for QEMU. */
    wr(R_STATUS, STAT_DAC1 | STAT_DAC2);

    /* Park the frame registers; nothing runs before play(). */
    set_dac1_page();
    wr(R_DAC1_FRAMEADR, (uint32_t)DMA_PHYS);
    wr(R_DAC1_FRAMECNT, 0u);
    wr(R_DAC1_SCOUNT,   0u);

    es_rate = dac1_rates[0];
    es_ready = 1;
    return 0;
}

int es1371_ready(void) { return es_ready ? 1 : 0; }

int es1371_play(const uint8_t *pcm, uint32_t len, uint32_t rate) {
    if (!es_ready) return -8;                  /* ENOSYS: no card / init  */
    if (!pcm || !len) return -22;              /* EINVAL                  */
    if (len > MAX_SAMPLES) return -22;         /* EINVAL: DMA too small   */
    if (rate < 4000u || rate > 48000u) return -22;

    /* Advance the software completion state machine before refusing a
     * second block: if the previous deadline already expired this clears
     * es_busy and lets the new play proceed.  No blocking. */
    if (es_busy) {
        (void)es1371_playing();
        if (es_busy) return -16;               /* EBUSY                   */
    }

    uint32_t sel    = quantize_rate(rate);
    uint32_t actual = dac1_rates[sel];

    /* 8-bit unsigned mono -> 16-bit stereo signed little-endian.
     * (pcm[i]-128) is -128..127; <<8 gives -32768..32512.  Each frame is
     * left then right int16, i.e. 4 bytes (note (4)).  i386 is LE, so a
     * volatile int16_t store already produces the little-endian bytes the
     * DAC1 DMA engine expects. */
    for (uint32_t i = 0; i < len; i++) {
        int16_t s = (int16_t)(((int)pcm[i] - 128) << 8);
        dma_buf[2u * i]      = s;              /* left  channel */
        dma_buf[2u * i + 1u] = s;              /* right channel */
    }

    es_rate = actual;
    es_start_tick = timer_ticks;
    /* Bounded software timeout.  frames == len (4-byte frames at the
     * actual DAC1 rate).  +2 ticks of slack so a slightly slow backend
     * still finishes; es1371_playing() halts the looped DMA at expiry. */
    es_play_ticks = (len * 100u) / actual + 2u;

    arm_dma(len, sel);

    es_busy = 1;
    es_last_err = 0;
    return 0;                                  /* 0 = DMA armed (async)   */
}

int es1371_playing(void) {
    if (!es_ready || !es_busy) return 0;

    /* READ ONLY: never spin waiting for timer_ticks (int 0x80 has IF=0).
     * Each poll from a live-interrupt context advances the timeout. */
    uint32_t elapsed = timer_ticks - es_start_tick;   /* unsigned wrap ok */
    if (elapsed >= es_play_ticks) {
        halt_dma();          /* QEMU loops forever otherwise (note (6))   */
        es_busy = 0;
        return 0;
    }
    return 1;
}

int es1371_stop(void) {
    if (!es_ready) return -8;
    halt_dma();
    es_busy = 0;
    return 0;
}

int es1371_irq(void) {
    if (!es_ready) return 0;

    uint32_t st   = rr(R_STATUS);
    uint32_t mine = st & (STAT_DAC1 | STAT_DAC2);
    if (!mine) return 0;       /* spurious / another device on the line */

    /* Real ES1370/ES1371: STATUS is write-1-to-clear.  QEMU has NO STATUS
     * write case (note (5)), so this is a no-op there but correct on
     * hardware / VMware. */
    wr(R_STATUS, mine);

    /* QEMU compatibility: es1370_maybe_lower_irq() only clears a STAT_DACx
     * bit and deasserts the PCI line when PnINTEN is cleared.  Clear those
     * bits so QEMU cannot hold the IRQ line high (storm).  play() rewrites
     * SERIAL_CONTROL on every start, so this never breaks playback. */
    uint32_t sctl = rr(R_SERIAL_CONTROL);
    wr(R_SERIAL_CONTROL, sctl & ~(SCTRL_P1INTEN | SCTRL_P2INTEN));

    es_irq_count++;
    /* NOTE: we deliberately do NOT clear es_busy here.  In looped mode a
     * DAC interrupt marks a period boundary, not end-of-block; completion
     * is decided by es1371_playing()'s software timeout.  The IRQ is only
     * an optional accelerator / diagnostic. */
    return 1;                  /* 1 = this card's interrupt */
}

int es1371_irq_line(void) {
    if (!es_probed) return -1;
    return (int)es_irq_line;
}

uint32_t es1371_io_base(void) { return (uint32_t)es_io; }

int es1371_info(int what) {
    switch (what) {
    case ES1371_INFO_READY:    return es_ready ? 1 : 0;
    /* Route PLAYING through es1371_playing() so polling the INFO selector
     * also advances the software completion state machine. */
    case ES1371_INFO_PLAYING:  return es1371_playing() ? 1 : 0;
    case ES1371_INFO_IRQS:     return (int)es_irq_count;
    case ES1371_INFO_ERROR:    return es_last_err;
    case ES1371_INFO_BUFSZ:    return (int)DMA_BYTES;
    case ES1371_INFO_RATE:     return (int)es_rate;
    case ES1371_INFO_IOBASE:   return (int)es_io;
    case ES1371_INFO_IRQ:      return es1371_irq_line();
    case ES1371_INFO_VENDOR:   return (int)es_vendor;
    case ES1371_INFO_DEVICE:   return (int)es_device;
    case ES1371_INFO_STATUS:   return es_probed ? (int)rr(R_STATUS) : 0;
    case ES1371_INFO_CONTROL:  return es_probed ? (int)rr(R_CONTROL) : 0;
    case ES1371_INFO_SERIAL:   return es_probed ? (int)rr(R_SERIAL_CONTROL) : 0;
    case ES1371_INFO_SERR:     return es_probed ? (int)rr(0x20) : 0;
    case ES1371_INFO_SCOUNT: {
        if (!es_probed) return 0;
        uint32_t saved = rr(R_MEMPAGE);
        set_dac1_page();
        uint32_t v = rr(R_DAC1_SCOUNT);
        wr(R_MEMPAGE, saved & 0xFu);
        return (int)v;
    }
    case ES1371_INFO_FRAMECNT: {
        if (!es_probed) return 0;
        /* Save/restore MEMPAGE so this diagnostic has no lasting side
         * effect on the paged window used by play(). */
        uint32_t saved = rr(R_MEMPAGE);
        set_dac1_page();
        uint32_t fc = rr(R_DAC1_FRAMECNT);
        wr(R_MEMPAGE, saved & 0xFu);        /* QEMU: mempage = val & 0xf */
        return (int)fc;
    }
    default:                   return -8;   /* ENOSYS */
    }
}

int es1371_rd_raw(int off) {
    if (!es_probed || off < 0 || off > 0x3C || (off & 3)) return -1;
    return (int)rr((uint8_t)off);
}

int es1371_tone(uint32_t freq_hz, uint32_t ms) {
    if (!es_ready) return -8;
    if (freq_hz == 0 || freq_hz > 20000u || ms == 0) return -22;

    /* 22050 is an exact DAC1 WTSRSEL rate, so no quantization skew. */
    uint32_t rate = 22050u;
    uint32_t n = (rate * ms) / 1000u;
    if (n > MAX_SAMPLES) n = MAX_SAMPLES;

    /* 16.16 fractional phase into sine32[], integer-only (no libm).
     * 0x80 is silence for 8-bit unsigned PCM; swing kept below full
     * scale, same as snd.c::snd_tone(). */
    uint32_t step  = (freq_hz << 16) / rate;
    uint32_t phase = 0;
    for (uint32_t i = 0; i < n; i++) {
        int s = (int)sine32[(phase >> 16) & 31u];
        tone_scratch[i] = (uint8_t)(128 + (s * 90) / 127);
        phase += step;
    }
    return es1371_play(tone_scratch, n, rate);
}
