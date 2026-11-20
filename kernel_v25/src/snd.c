/* snd.c -- Sound Blaster 16 (ISA) 8-bit PCM playback for CatOS.
 *
 * Driver shape
 * ------------
 *   probe : reset the DSP, expect 0xAA, read the version with 0xE1,
 *           cache the mixer IRQ/DMA bitmaps, unmask IRQ5.
 *   play  : copy 8-bit unsigned mono samples into the low-memory ISA DMA
 *           buffer, program 8237 channel 1, then issue 0x41 (rate) and
 *           0x14 (single-cycle DAC, block length - 1).
 *   irq   : reading iobase+0x0E acknowledges the 8-bit DMA interrupt;
 *           clear the busy flag.  PIC EOI stays in intr.c.
 *
 * Deliberate constraints
 * ----------------------
 * * No floating point (the kernel is built -mno-80387): the sine is a
 *   32-entry integer table with a 16.16 phase accumulator.
 * * Every port poll is bounded by a fixed loop count, so a machine with
 *   no SB16 fails the probe in microseconds instead of hanging the boot.
 * * All DSP writes tolerate a stuck "busy" bit: QEMU implements offset
 *   0x0C as a read-status mirror, so treating it as a hard gate would
 *   wedge the driver on the very platform used for regression.
 * * Playback is asynchronous.  See the note in snd.h about int 0x80
 *   entering with IF=0 -- no waiting inside a syscall.
 */

#include "snd.h"
#include "io.h"
#include "intr.h"      /* intr_unmask_irq() for the PCI audio IRQ line */
#include "es1371.h"    /* v2.21 #56: VMware / QEMU ES1370 PCI backend  */

/* ---- DSP / mixer ports (base 0x220) ----------------------------------- */
#define DSP_RESET    (SND_IOBASE + 0x06)
#define DSP_READ     (SND_IOBASE + 0x0A)
#define DSP_WRITE    (SND_IOBASE + 0x0C)
#define DSP_RSTATUS  (SND_IOBASE + 0x0E)   /* bit7 = data available        */
#define DSP_ACK8     (SND_IOBASE + 0x0E)   /* read also acks 8-bit DMA IRQ */
#define MIXER_INDEX  (SND_IOBASE + 0x04)
#define MIXER_DATA   (SND_IOBASE + 0x05)

/* ---- 8237 DMA controller, 8-bit channel 1 ---------------------------- */
#define DMA_MASK_ALL 0x0Au     /* single-channel mask register            */
#define DMA_FLIPFLOP 0x0Cu    /* clear byte-pointer flip-flop             */
#define DMA_MODE     0x0Bu
#define DMA_ADDR     0x02u     /* channel 1 address                       */
#define DMA_COUNT    0x03u     /* channel 1 count                         */
#define DMA_PAGE     0x83u     /* channel 1 page register                 */
#define DMA_MODE_CH1 0x49u     /* single / read(mem->I/O) / incr / ch1    */
#define DMA_MASK_CH1 0x05u     /* set mask bit for channel 1              */
#define DMA_UNMASK_1 0x01u     /* clear mask bit for channel 1            */

#define PIC_MASTER_IMR 0x21u

/* Bounded polls: on a machine without SB16 these finish immediately
 * (unmapped ISA ports read back as 0xFF and fail the 0xAA check). */
#define DSP_POLL  20000u
#define DSP_GAP   128u

/* The ISA DMA window.  It is identity-mapped and never handed to the
 * frame allocator (see paging.c), so the physical address is also a
 * directly usable kernel pointer. */
static uint8_t *const snd_buf = (uint8_t *)SND_DMA_PHYS;

static volatile int      snd_up;         /* any backend ready             */
static volatile int      snd_busy;       /* DMA block in flight            */
static volatile uint32_t snd_irq_count;  /* acknowledged DMA completions   */
static volatile int      snd_last_err;   /* last error code, 0 = none      */
/* v2.21: 0 = none, 1 = SB16 (ISA), 2 = ES1370/1371 (PCI). */
static int snd_backend;
static const char *const snd_backend_names[3] = {
    "none", "SB16 (ISA)", "ES1370/1371 (PCI)"
};

const char *snd_backend_name(void) {
    return snd_backend_names[snd_backend >= 0 && snd_backend <= 2 ? snd_backend : 0];
}
static uint16_t          snd_version;
static uint32_t          snd_rate = SND_RATE_DEFAULT;
static uint8_t           snd_irq_map;    /* mixer 0x80                    */
static uint8_t           snd_dma_map;    /* mixer 0x81                    */

/* 32 samples of a full sine period, amplitude +/-127.  Integer-only. */
static const int8_t sine32[32] = {
      0,   25,   49,   71,   90,  106,  117,  125,
    127,  125,  117,  106,   90,   71,   49,   25,
      0,  -25,  -49,  -71,  -90, -106, -117, -125,
   -127, -125, -117, -106,  -90,  -71,  -49,  -25
};

/* ---- low-level port helpers ------------------------------------------- */

static void dsp_write(uint8_t v) {
    /* Wait for the write buffer if it really is busy, but never fail: a
     * constant 0x80 on offset 0x0C (QEMU's read-status mirror) must not
     * wedge the driver.  A short gap between bytes is enough at the
     * command rates this driver uses. */
    for (uint32_t i = 0; i < DSP_GAP && (inb(DSP_WRITE) & 0x80u); i++)
        io_wait();
    for (volatile uint32_t i = 0; i < 8; i++) io_wait();
    outb(DSP_WRITE, v);
}

static int dsp_read(uint8_t *out) {
    for (uint32_t i = 0; i < DSP_POLL; i++) {
        if (inb(DSP_RSTATUS) & 0x80u) {
            *out = inb(DSP_READ);
            return 0;
        }
        io_wait();
    }
    return -1;
}

static uint8_t mixer_read(uint8_t idx) {
    outb(MIXER_INDEX, idx);
    for (volatile uint32_t i = 0; i < 16; i++) io_wait();
    return inb(MIXER_DATA);
}

/* v2.65: shell-facing mixer read.  SB16 has a real register set; the PCI
 * backend does not implement AC97 codec access, so report "unavailable"
 * rather than inventing values. */
int snd_mixer_read(int reg) {
    if (!snd_up || snd_backend != 1) return -1;
    if (reg < 0 || reg > 255) return -1;
    return (int)mixer_read((uint8_t)reg);
}

static void mixer_write(uint8_t idx, uint8_t val) {
    outb(MIXER_INDEX, idx);
    for (volatile uint32_t i = 0; i < 16; i++) io_wait();
    outb(MIXER_DATA, val);
}

/* ---- 8237 channel 1 --------------------------------------------------- */

static void dma8_arm(uint32_t phys, uint32_t len) {
    uint32_t cnt = len - 1;
    outb(DMA_MASK_ALL, DMA_MASK_CH1);       /* mask channel 1             */
    outb(DMA_FLIPFLOP, 0x00);             /* reset byte pointer          */
    outb(DMA_MODE, DMA_MODE_CH1);          /* single, read, auto-reload off */
    outb(DMA_ADDR,  (uint8_t)(phys & 0xFFu));
    outb(DMA_ADDR,  (uint8_t)((phys >> 8) & 0xFFu));
    outb(DMA_PAGE,  (uint8_t)((phys >> 16) & 0xFFu));
    outb(DMA_COUNT, (uint8_t)(cnt & 0xFFu));
    outb(DMA_COUNT, (uint8_t)((cnt >> 8) & 0xFFu));
    outb(DMA_MASK_ALL, DMA_UNMASK_1);       /* unmask channel 1           */
}

static void dma8_mask(void) { outb(DMA_MASK_ALL, DMA_MASK_CH1); }

/* ---- public API -------------------------------------------------------- */

/* ---- backend 1: Sound Blaster 16 (ISA) -------------------------------- */
static int sb16_init(void) {
    uint8_t b = 0, lo = 0, hi = 0;

    snd_up = 0;
    snd_busy = 0;
    snd_last_err = 0;

    /* DSP reset: hold 1 for >= 3 us, then drop. */
    outb(DSP_RESET, 1);
    for (volatile uint32_t i = 0; i < 16; i++) io_wait();
    outb(DSP_RESET, 0);

    /* The DSP answers with 0xAA.  QEMU queues it immediately; real parts
     * can take ~100 us, hence the bounded poll rather than a fixed delay. */
    int found = 0;
    for (int tries = 0; tries < 32; tries++) {
        if (dsp_read(&b) != 0) break;
        if (b == 0xAAu) { found = 1; break; }
    }
    if (!found) { snd_last_err = 1; return -1; }   /* no SB16 present */

    /* 0xE1 returns minor then major on QEMU (ver & 0xff, ver >> 8).
     * Store it the conventional way: 0xMMmm. */
    dsp_write(0xE1);
    if (dsp_read(&lo) != 0 || dsp_read(&hi) != 0) {
        snd_last_err = 2;
        return -2;
    }
    snd_version = (uint16_t)(((uint16_t)hi << 8) | lo);

    /* Master volume to maximum; speaker on. */
    mixer_write(0x22, 0xFFu);
    dsp_write(0xD1);

    /* Diagnostic readback: QEMU seeds mixer 0x80 (IRQ) and 0x81 (DMA). */
    snd_irq_map = mixer_read(0x80);
    snd_dma_map = mixer_read(0x81);

    /* intr_enable() left IRQ5 masked; open just that line.  The IRQ5 case
     * in intr.c is inert until snd_up is set below. */
    outb(PIC_MASTER_IMR, inb(PIC_MASTER_IMR) & ~(uint8_t)(1u << SND_IRQ_LINE));

    snd_rate = SND_RATE_DEFAULT;
    snd_up = 1;
    return 0;
}

/* ---- v2.21: pick a backend once, at boot -------------------------------
 * SB16 first (all existing QEMU audio regressions use it).  If the ISA
 * DSP does not answer, fall back to the ES1370/ES1371 PCI card that VMware
 * exposes.  A machine with neither card must fail fast so boot continues
 * with sound simply disabled. */
int snd_init(void) {
    snd_backend = 0;
    snd_up = 0;
    snd_busy = 0;
    snd_last_err = 0;
    snd_irq_count = 0;

    if (sb16_init() == 0) {
        snd_backend = 1;
        return 0;
    }
    if (es1371_probe() && es1371_init() == 0) {
        snd_backend = 2;
        snd_up = 1;
        snd_rate = SND_RATE_DEFAULT;
        snd_last_err = 0;
        int irq = es1371_irq_line();
        if (irq >= 0) intr_unmask_irq(irq);
        return 0;
    }
    return -1;
}

void snd_irq(void) {
    /* Only the SB16 backend owns IRQ5's DSP ack.  The ES1371 backend is
     * serviced directly by es1371_irq() from intr.c (it checks STATUS
     * itself), so a shared PCI line never clears a bogus SB16 busy flag. */
    if (snd_backend != 1 || !snd_up) return;
    /* Reading the status port acks mixer 0x82 bit0 and lowers the PIC line
     * on QEMU; on real hardware it is the documented 8-bit DMA ack. */
    (void)inb(DSP_ACK8);
    snd_irq_count++;
    snd_busy = 0;
}

int snd_info(int what) {
    if (what == SND_INFO_BACKEND) return snd_backend;
    if (what == SND_INFO_DEVICE)
        return snd_backend == 2 ? es1371_info(ES1371_INFO_DEVICE) : 0;
    if (snd_backend == 2) {
        switch (what) {
        case SND_INFO_READY:   return es1371_ready();
        case SND_INFO_VERSION: return es1371_info(ES1371_INFO_DEVICE);
        case SND_INFO_PLAYING: return es1371_playing();
        case SND_INFO_IRQS:    return es1371_info(ES1371_INFO_IRQS);
        case SND_INFO_ERROR:   return es1371_info(ES1371_INFO_ERROR);
        /* Callers size their 8-bit mono buffer from this; the PCI card
         * needs 4 bytes per sample after widening, so advertise samples. */
        case SND_INFO_BUFSZ:   return (int)(ES1371_DMA_BUF_SZ / 4u);
        case SND_INFO_RATE:    return es1371_info(ES1371_INFO_RATE);
        case SND_INFO_IRQSET:  return es1371_info(ES1371_INFO_IRQ);
        case SND_INFO_DMASET:  return (int)es1371_info(ES1371_INFO_IOBASE);
        default:               return -8;
        }
    }
    switch (what) {
    case SND_INFO_READY:   return snd_up ? 1 : 0;
    case SND_INFO_VERSION: return (int)snd_version;
    case SND_INFO_PLAYING: return snd_busy ? 1 : 0;
    case SND_INFO_IRQS:    return (int)snd_irq_count;
    case SND_INFO_ERROR:   return snd_last_err;
    case SND_INFO_BUFSZ:   return (int)SND_DMA_BUF_SZ;
    case SND_INFO_RATE:    return (int)snd_rate;
    case SND_INFO_IRQSET:  return (int)snd_irq_map;
    case SND_INFO_DMASET:  return (int)snd_dma_map;
    default:               return -8;               /* ENOSYS */
    }
}

uint8_t *snd_dma_buffer(void) { return snd_buf; }

int snd_play(const uint8_t *pcm, uint32_t len, uint32_t rate) {
    if (snd_backend == 2) {
        if (!pcm || !len) return -22;
        int r = es1371_play(pcm, len, rate);
        if (r == 0) snd_rate = rate;
        return r;
    }
    if (!snd_up)        return -8;    /* ENOSYS: no SB16 */
    if (!pcm || !len)   return -22;   /* EINVAL */
    if (len > SND_DMA_BUF_SZ) return -22;
    if (rate < 5000u || rate > 45000u) return -22;  /* SB16/QEMU range */
    if (snd_busy)       return -16;   /* EBUSY */

    if (pcm != snd_buf)
        for (uint32_t i = 0; i < len; i++) snd_buf[i] = pcm[i];

    /* Halt anything left over from a previous (possibly aborted) block. */
    dsp_write(0xD0);
    dma8_mask();

    snd_busy = 1;
    snd_rate = rate;
    snd_last_err = 0;

    dma8_arm(SND_DMA_PHYS, len);

    /* 0x41: output rate, high byte first.  0x14: 8-bit single-cycle DMA
     * DAC, followed by (block length - 1) low byte then high byte.
     * QEMU's complete() turns that into dma_cmd8(s, 0, len). */
    dsp_write(0x41);
    dsp_write((uint8_t)(rate >> 8));
    dsp_write((uint8_t)(rate & 0xFFu));
    dsp_write(0xD1);                       /* speaker on */
    dsp_write(0x14);
    dsp_write((uint8_t)((len - 1) & 0xFFu));
    dsp_write((uint8_t)(((len - 1) >> 8) & 0xFFu));
    return 0;
}

int snd_stop(void) {
    if (snd_backend == 2) return es1371_stop();
    if (!snd_up) return -8;
    dsp_write(0xD0);                       /* halt 8-bit DMA */
    dma8_mask();
    snd_busy = 0;
    return 0;
}

int snd_tone(uint32_t freq_hz, uint32_t ms) {
    if (snd_backend == 2) return es1371_tone(freq_hz, ms);
    if (!snd_up) return -8;
    if (freq_hz == 0 || freq_hz > 20000u || ms == 0) return -22;

    uint32_t rate = SND_RATE_DEFAULT;
    uint32_t n = (rate * ms) / 1000u;
    if (n > SND_DMA_BUF_SZ) n = SND_DMA_BUF_SZ;

    /* 16.16 fractional index into sine32[].  8-bit unsigned PCM: 0x80
     * is silence, so the swing is deliberately kept below full scale. */
    uint32_t step = (freq_hz << 16) / rate;
    uint32_t phase = 0;
    for (uint32_t i = 0; i < n; i++) {
        int s = (int)sine32[(phase >> 16) & 31u];
        snd_buf[i] = (uint8_t)(128 + (s * 90) / 127);
        phase += step;
    }
    return snd_play(snd_buf, n, rate);
}
