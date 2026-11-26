/* es1371.h -- Ensoniq ES1370 / ES1371 PCI audio driver for CatOS.
 *
 * Devices
 * -------
 *   QEMU   : -device es1370        -> 1274:5000 (ENSONIQ ES1370)
 *   VMware : virtual "ES1371"      -> 1274:1371 (AC97 codec variant)
 * Both IDs are accepted.  BAR0 is an I/O-space BAR (256 bytes), so every
 * register is reached with inl/outl(io_base + off): no high-MMIO mapping
 * is needed, unlike the SVGA / e1000 devices.
 *
 * Programming model (verified against QEMU v8.2.0 hw/audio/es1370.c)
 * ------------------------------------------------------------------
 *   probe : raw 0xCF8/0xCFC PCI config scan for 1274:5000 / 1274:1371;
 *           latch BAR0 I/O base, IRQ line (0x3C), enable IO + bus-master.
 *   play  : convert 8-bit unsigned mono PCM into 16-bit stereo signed LE
 *           inside a static low-memory DMA buffer, program the DAC1 frame
 *           registers (through the MEMPAGE=0xc paged window) and raise
 *           CTRL_DAC1_EN.  Returns immediately (asynchronous).
 *   done  : QEMU implements *only* looped DMA (there is no "stop at end
 *           of buffer" mode), so completion is detected in software by a
 *           bounded timer-based timeout; es1371_playing() latches the
 *           completion and halts the channel.  No blocking inside a
 *           syscall: int 0x80 enters through an interrupt gate (IF=0).
 *   irq   : optional top half.  Return 1 when this card's STATUS has a
 *           pending DAC bit.  The PIC EOI is sent centrally by intr.c.
 *
 * Why 8-bit mono -> 16-bit stereo: the DAC1 DMA engine is programmed for
 * P1FMT = 16-bit stereo, i.e. a 4-byte little-endian frame (left int16,
 * right int16).  snd.c / the syscall ABI hand us 8-bit unsigned mono, so
 * es1371_play() must widen and duplicate each sample; feeding the raw
 * 8-bit bytes to the DMA engine would produce noise at double speed.
 *
 * See es1371.c for the register/bit notes that were cross-checked against
 * the QEMU source (MEMPAGE window, FRAMECNT half-word order, and the fact
 * that QEMU does NOT implement write-1-to-clear on STATUS).
 */
#ifndef ES1371_H
#define ES1371_H

#include <stdint.h>

/* ---- es1371_info(what) selectors ---- */
#define ES1371_INFO_READY     0  /* 1 = probed + init ok, ready to play    */
#define ES1371_INFO_PLAYING   1  /* 1 = DMA/software playback still active */
#define ES1371_INFO_IRQS      2  /* count of acknowledged ES1370 IRQs      */
#define ES1371_INFO_ERROR     3  /* last driver error code, 0 = none       */
#define ES1371_INFO_BUFSZ     4  /* DMA buffer capacity in bytes           */
#define ES1371_INFO_RATE      5  /* sample rate of the last play request   */
#define ES1371_INFO_IOBASE    6  /* BAR0 I/O base (diagnostic)             */
#define ES1371_INFO_IRQ       7  /* PCI IRQ line, -1 if not probed         */
#define ES1371_INFO_VENDOR    8  /* PCI vendor id (0x1274)                 */
#define ES1371_INFO_DEVICE    9  /* PCI device id (0x5000 / 0x1371)        */
#define ES1371_INFO_FRAMECNT 10  /* raw DAC1_FRAMECNT readback             */
#define ES1371_INFO_STATUS   11  /* raw STATUS readback                    */
/* v2.72 diagnostics: the raw values VMware support needs to tell a
 * register-map bug apart from a muted AK4531 codec. */
#define ES1371_INFO_CONTROL  12  /* raw CONTROL readback                   */
#define ES1371_INFO_SERIAL   13  /* raw SERIAL_CONTROL readback            */
#define ES1371_INFO_SCOUNT   14  /* raw DAC1_SCOUNT (paged) readback       */
#define ES1371_INFO_SERR     15  /* raw 0x20 SERR/status readback          */
/* v2.82 D6: PCM diagnostics for sndinfo.  PERIOD/AVAIL are decoded from the
 * real FRAMECNT/SCOUNT readbacks; UNDERRUN stays -1 while the driver does not
 * track xruns (callers report n/a instead of inventing a zero counter). */
#define ES1371_INFO_PERIOD    16  /* frames in one DMA block, -1 if absent  */
#define ES1371_INFO_AVAIL     17  /* frames left in current block, -1 n/a   */
#define ES1371_INFO_UNDERRUN  18  /* xrun counter, -1 = not exported        */

/* ---- hardware geometry ---- */
#define ES1371_DMA_PHYS      0x550000u  /* 64 KiB aligned, identity-mapped  */
#define ES1371_DMA_BUF_SZ    65536u     /* one 64 KiB page, no boundary xing*/

/* Probe + init.  Must run AFTER intr_enable().  A missing card is NOT
 * fatal: probe returns 0 quickly so the no-audio QEMU regressions keep
 * booting.  es1371_probe() is idempotent. */
int      es1371_probe(void);
int      es1371_init(void);
int      es1371_ready(void);

/* Start an 8-bit unsigned mono PCM block.  Asynchronous: arms DMA and
 * returns 0 at once.  'rate' is quantized to the DAC1 WTSRSEL table
 * {5512, 11025, 22050, 44100}; the nearest rate is used. */
int      es1371_play(const uint8_t *pcm, uint32_t len, uint32_t rate);
int      es1371_playing(void);
int      es1371_stop(void);

/* IRQ top half (1 = this card's interrupt) and the PCI IRQ line the caller
 * must unmask; the central isr_irq() sends the PIC EOI. */
int      es1371_irq(void);
int      es1371_irq_line(void);

int      es1371_info(int what);
/* v2.72: read-only raw dword at register offset `off` (0x00..0x3C).
 * Pure readback: no write, no side effect, safe on any ES137x part. */
int      es1371_rd_raw(int off);
uint32_t es1371_io_base(void);

/* Integer sine into the internal DMA buffer, then es1371_play(). */
int      es1371_tone(uint32_t freq_hz, uint32_t ms);

#endif /* ES1371_H */
