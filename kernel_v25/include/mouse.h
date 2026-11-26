#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>

/*
 * mouse.h -- v2.57 PS/2 auxiliary (mouse) driver on the 8042 controller.
 *
 * The 8042 shares one data port (0x60) between the keyboard (IRQ1) and the
 * auxiliary device (IRQ12).  Status port 0x64 bit 5 distinguishes the
 * source: set = byte waiting for the aux path, clear = keyboard byte.
 * Both IRQ top halves drain the controller through the same routing
 * helper, so a mouse byte can never be consumed as a scancode and vice
 * versa.
 *
 * Packets: standard PS/2 = 3 bytes; IntelliMouse (wheel) = 4 bytes, the
 * last one carrying the signed wheel notch in its low nibble.
 *   byte 0: bit0 L, bit1 R, bit2 M, bit3=1, bit4 X-sign, bit5 Y-sign,
 *           bit6 X-overflow, bit7 Y-overflow
 *   byte 1: X delta (sign extended by byte0 bit4)
 *   byte 2: Y delta (sign extended by byte0 bit5)
 *   byte 3: wheel delta, signed low nibble (4-byte mode only)
 */

/* Button masks as they arrive in the PS/2 packet byte0 (and as the QEMU
 * HMP `mouse_button` mask passes them through): 1 = left, 2 = middle,
 * 4 = right. */
#define MOUSE_BTN_LEFT   0x01
#define MOUSE_BTN_MID    0x02
#define MOUSE_BTN_RIGHT  0x04

/* One drained movement / button / wheel event.  Coordinates are the
 * driver's maintained ABSOLUTE pixel position at event time; dx/dy are
 * this packet's relative deltas, dz the wheel notch (0 = no wheel). */
typedef struct {
    int      x, y;          /* absolute pixel position (clamped)       */
    int      dx, dy;        /* relative deltas this packet             */
    int      dz;            /* wheel notch, signed (3-byte mode: 0)     */
    uint8_t  buttons;       /* live button mask MOUSE_BTN_*            */
    uint8_t  changed;       /* button mask that changed this packet    */
    uint8_t  down;          /* 1 = press edge, 0 = release edge        */
} mouse_event_t;

/* Boot-time bring-up: enable the aux port, turn IRQ12 on in the 8042
 * config byte, negotiate IntelliMouse (4-byte wheel) mode, unmask IRQ12
 * through the PIC.  Safe when no PS/2 mouse is attached: the controller
 * commands are acknowledged by an absent mouse, polling just sees noise. */
void mouse_init(void);

/* IRQ12 top half: drain aux bytes, reassemble packets, enqueue events. */
void mouse_irq(void);

/* Shared 8042 drain used by IRQ1 (keyboard) and IRQ12 (mouse): one status
 * bit5 lookup routes each pending 0x60 byte to the correct consumer. */
void ps2_drain(void);

/* Non-blocking event pull.  Returns 1 when `e` holds the next event. */
int  mouse_poll_event(mouse_event_t *e);

/* Snapshot the live absolute position / button mask. */
void mouse_get_state(int *x, int *y, uint8_t *buttons);

/* Clamp the maintained position to the active pixel surface.  Called by
 * the console glue when the display mode / cell metrics change. */
void mouse_clamp(int px_w, int px_h);

/* Reset driver-side session state between desktop lifetimes: drains the
 * queued event ring, clears the live button bitmap and any partial packet,
 * but keeps the maintained absolute position.  It does NOT time-release a
 * button held down during a legitimate long drag. */
void mouse_state_reset(void);
void mouse_dev_info(int *id, int *wheel, int *pkt);   /* v2.73 diagnostics */

/* v2.62: wheel notches buffered by the IRQ12 top half.  The console drains
 * them from con_tick() at a render-safe point, so scrolling works even while
 * the shell is blocked waiting on a foreground task (the old code only
 * drained wheel events from the keyboard read loop).  Returns the signed sum
 * and clears it. */
int  mouse_take_wheel(void);

/* Diagnostics: number of assembled packets since boot (IRQ12/aux drain). */
uint32_t mouse_packet_count(void);
uint32_t mouse_irq_count(void);
uint32_t mouse_aux_bytes(void);
uint32_t mouse_kbd_bytes_irq12(void);

#endif /* MOUSE_H */
