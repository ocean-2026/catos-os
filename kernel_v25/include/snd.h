/* snd.h -- 8-bit unsigned MONO PCM audio facade, CE ABI v2.11.
 * v2.21: snd.c is now a two-backend DISPATCHER.  The public ABI is
 * unchanged: callers always hand over 8-bit unsigned mono PCM and get an
 * asynchronous play.  Backend 1 is SB16 (ISA); backend 2 is the Ensoniq
 * ES1370/ES1371 PCI card (see es1371.h), which widens the samples to
 * 16-bit stereo internally.  Selection happens once in snd_init():
 * SB16 first, ES1371 as fallback.  No new syscall slots were added.
 *
 * Why SB16 was historically chosen and not a PCI card: CatOS has raw 0xCF8/0xCFC config-space
 * access (hw.c / net.c / be_vbe.c) but no generic PCI driver layer and no
 * PCI interrupt routing.  Its only IRQ hook is a hard-coded switch in
 * intr.c, and intr_enable() unmasks just IRQ0/1/2.  SB16 is an ISA device
 * at a fixed iobase with a fixed IRQ line, so it needs exactly one new
 * switch case and one IMR bit -- no PCI enumeration, no bus-master
 * descriptor rings, no MSI.
 *
 * Programming model (QEMU hw/audio/sb16.c and real DSP 4.xx agree here):
 *   reset  -> DSP queues 0xAA
 *   0xE1   -> two version bytes
 *   0x41   -> output sample rate (high byte first)
 *   0x14   -> 8-bit single-cycle DMA DAC, (len-1) low byte then high byte
 *   0xD1   -> speaker on / 0xD3 off, 0xD0 halt / 0xD4 continue 8-bit DMA
 *   read iobase+0x0E acknowledges the 8-bit DMA interrupt (mixer 0x82 bit0)
 *
 * The DMA buffer must live below 16 MiB (the 8237 page register is 8 bit)
 * and must not cross a 64 KiB boundary.  0x520000 is 64 KiB aligned and is
 * outside the PCnet ring reservation at 0x500000..0x510000; the kernel
 * identity-maps the low 64 MiB, so that physical address is also a valid
 * kernel pointer (the same trick net.c uses for its descriptor rings).
 *
 * Append-only ABI: slots 57..60 are added at the END of catos_api_t, so
 * every existing .ce binary keeps its old slot indices.
 */
#ifndef SND_H
#define SND_H

#include <stdint.h>

/* ---- hardware geometry (QEMU sb16 defaults == factory defaults) ------- */
#define SND_IOBASE        0x220u
#define SND_IRQ_LINE      5
#define SND_DMA8_CHAN     1
#define SND_RATE_DEFAULT  22050u
#define SND_DMA_BUF_SZ    32768u
#define SND_DMA_PHYS      0x520000u

/* ---- snd_info(what) selectors (shared with ring3; see ce_abi.h) ------ */
#define SND_INFO_READY    0   /* 1 = SB16 probed and IRQ5 unmasked        */
#define SND_INFO_VERSION  1   /* DSP version as 0xMMmm (major<<8|minor)   */
#define SND_INFO_PLAYING  2   /* 1 = a DMA block is still in flight        */
#define SND_INFO_IRQS     3   /* count of acknowledged 8-bit DMA IRQs      */
#define SND_INFO_ERROR    4   /* last driver error code (0 = none)         */
#define SND_INFO_BUFSZ    5   /* DMA buffer capacity in bytes              */
#define SND_INFO_RATE     6   /* sample rate of the last play request      */
#define SND_INFO_IRQSET   7   /* mixer 0x80 IRQ bitmap (diagnostic)        */
#define SND_INFO_DMASET   8   /* mixer 0x81 DMA bitmap (diagnostic)        */
/* v2.21 #56: backend selectors (append-only, old slots unchanged) */
#define SND_INFO_BACKEND  9   /* 0=none 1=SB16(ISA) 2=ES1370/1371(PCI)   */
#define SND_INFO_DEVICE  10   /* PCI device id for backend 2, else 0      */

const char *snd_backend_name(void);
/* v2.65: read one mixer register from the active backend.  SB16 exposes
 * its real mixer (reg 0x22 = master playback L, 0x23 = R, 0x28 = mic,
 * 0x2E = speaker, ...).  The ES1370/1371 path has no AC97 codec access in
 * this kernel yet, so it consistently returns -1 ("not available").
 * New API -- no ABI slot consumed (ring0 builtin use only). */
int      snd_mixer_read(int reg);

/* Probe + init.  Must be called AFTER intr_enable() (it unmasks IRQ5) and
 * BEFORE shell_run() (which never returns).  0 = ready, negative = absent.
 * A missing device is NOT fatal: it must return quickly so the existing
 * no-audio QEMU regressions keep booting normally. */
int      snd_init(void);

/* IRQ5 top half, called from intr.c's hard-coded dispatch.  Must stay
 * short: ack the DSP, latch the completion flag, return.  The PIC EOI is
 * sent centrally by isr_irq(). */
void     snd_irq(void);

int      snd_info(int what);
/* Start an 8-bit unsigned mono PCM block.  Asynchronous: returns as soon
 * as the DMA is armed, because int 0x80 enters through an INTERRUPT GATE
 * (IF=0) -- waiting inside a syscall would deadlock (hlt would never wake
 * and timer_ticks would stop advancing).  Callers poll SND_INFO_PLAYING
 * from user/kernel context where interrupts are live. */
int      snd_play(const uint8_t *pcm, uint32_t len, uint32_t rate);
int      snd_stop(void);
/* Fill the DMA buffer with an integer sine and start it. */
int      snd_tone(uint32_t freq_hz, uint32_t ms);
/* Expose the low-memory buffer so the syscall can copy_from_user() into it. */
uint8_t *snd_dma_buffer(void);

#endif /* SND_H */
