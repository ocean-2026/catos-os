#ifndef CATOS_SVGA_H
#define CATOS_SVGA_H

/*
 * svga.h -- VMware SVGA II framebuffer driver (ABI v2.20).
 *
 * QEMU's "-vga std" exposes a Bochs dispi LFB (be_vbe / be_vbe24 already
 * handle it).  VMware Workstation, and QEMU's "-vga vmware", expose the
 * VMware SVGA II PCI device (15AD:0405) instead: registers through an
 * index/value I/O-port pair in BAR0, framebuffer in BAR1, command FIFO in
 * BAR2.  There is NO dispi port, which is why `vbe` / `gfx24` used to
 * report "no VBE/Bochs-dispi device found" on VMware.
 *
 * SVGA II only guarantees a 32bpp guest framebuffer (XRGB, 24-bit colour
 * depth + one unused/opaque byte).  CatOS therefore keeps its RGB24 public
 * drawing API and writes BGR + 0xFF into a 4-byte pixel on this device.
 * `GFX_INFO_HW_BPP` reports the real hardware depth; `GFX_INFO_BPP` stays
 * 24 because the colour format exposed to applications is unchanged.
 *
 * Direct framebuffer writes are NOT visible on SVGA II once the FIFO is
 * enabled (the host stops VGA dirty tracking).  svga_tick() issues a
 * throttled full-screen SVGA_CMD_UPDATE through the FIFO from the PIT IRQ.
 */
#include <stdint.h>

typedef struct {
    uint32_t phys;        /* framebuffer physical base (identity mapped)  */
    uint32_t bytes;       /* usable framebuffer bytes                     */
    uint32_t pitch;       /* bytes between scanlines (from the device)    */
    uint32_t w, h;
    uint8_t  bpp;         /* hardware bits per pixel (32, or 24 if lucky) */
    uint8_t  stride;      /* bytes per pixel (3 or 4)                     */
    uint8_t  off_r, off_g, off_b, off_x;  /* byte offsets inside a pixel */
} svga_surface_t;

/* 1 when a usable SVGA II PCI function was found (does NOT change mode). */
int      svga_probe(void);
/* Set an 800x600 mode and hand back the mapped surface.  Returns 0, or a
 * negative code.  On failure the device is put back into VGA text mode. */
int      svga_mode_set(int w, int h, svga_surface_t *out);
/* Disable SVGA (ENABLE=0, CONFIG_DONE=0) so be_text can reprogram VGA. */
void     svga_mode_off(void);
int      svga_active(void);
/* PIT IRQ0 hook: 25 Hz full-screen FIFO UPDATE while an SVGA mode is live. */
void     svga_tick(void);
/* Immediate full-screen UPDATE (used right after a mode switch / demo). */
void     svga_flush(void);

/* Diagnostic read-out for the `svgainfo` shell command. */
enum {
    SVGA_INFO_PRESENT = 0,
    SVGA_INFO_ACTIVE,
    SVGA_INFO_ID,
    SVGA_INFO_CAPS,
    SVGA_INFO_WIDTH,
    SVGA_INFO_HEIGHT,
    SVGA_INFO_BPP,
    SVGA_INFO_PITCH,
    SVGA_INFO_FB_PHYS,
    SVGA_INFO_FB_BYTES,
    SVGA_INFO_FIFO_PHYS,
    SVGA_INFO_FIFO_BYTES,
    SVGA_INFO_IO_BASE,
    SVGA_INFO_FLUSHES
};
uint32_t svga_info(int what);

#endif /* CATOS_SVGA_H */
