/*
 * mouse.c -- v2.57 PS/2 auxiliary (mouse) driver, IRQ12 top half.
 *
 * Layout matches the existing interrupt-driven keyboard design: the ISR
 * only drains the 8042, reassembles the raw packet and enqueues an event;
 * all console-side work (selection, wheel scrolling, paste) happens in
 * normal kernel context when the console glue polls the queue.
 *
 * Boot sequence (standard 8042 bring-up):
 *   1. disable kbd + aux ports, flush the output buffer
 *   2. config byte: IRQ12 enabled (bit1), aux port not held disabled (bit5)
 *   3. enable aux port (0xA8)
 *   4. IntelliMouse negotiation: sample rate 200/100/80, identify device;
 *      an ID of 3 means the mouse speaks 4-byte wheel packets
 *   5. defaults + enable streaming, unmask IRQ12 at the PIC
 */
#include "mouse.h"
#include "io.h"
#include "intr.h"
#include "keyboard.h"
#include "task.h"

#define PS2_DATA   0x60
#define PS2_STATUS 0x64
#define PS2_CMD    0x64

/* 8042 status register bits */
#define ST_OUT_FULL  0x01
#define ST_IN_FULL   0x02
#define ST_AUX_DATA  0x20

/* Default clamp surface: gfx24 is 800x600; the console glue re-clamps on
 * every mode switch via mouse_clamp(). */
#define MOUSE_MAX_X  799
#define MOUSE_MAX_Y  599

#define EVQ_SZ 32
static mouse_event_t evq[EVQ_SZ];
static volatile uint32_t evq_head, evq_tail;

static int      abs_x = 400, abs_y = 300;
static uint8_t  last_buttons;
static int      wheel_mode;            /* 1 = 4-byte IntelliMouse packets */
static volatile uint32_t pkt_count;    /* assembled packets (diagnostics)  */
static volatile uint32_t irq_count;    /* IRQ12 entries (diagnostics) */
static volatile uint32_t aux_bytes;    /* aux bytes drained (diagnostics) */
static volatile uint32_t kbd_bytes_irq12; /* kbd-routed bytes on line 12 */

/* packet reassembly */
static uint8_t  pkt[4];
static int      phase;
static int      pkt_size = 3;

/* ---- 8042 helpers ------------------------------------------------------- */

static void wait_in_empty(void) {
    int guard = 100000;
    while ((inb(PS2_STATUS) & ST_IN_FULL) && --guard > 0) { }
}
static void wait_out_full(void) {
    int guard = 100000;
    while (!(inb(PS2_STATUS) & ST_OUT_FULL) && --guard > 0) { }
}

static void aux_write(uint8_t cmd) {
    wait_in_empty();
    outb(PS2_CMD, 0xD4);               /* next 0x60 byte goes to aux */
    wait_in_empty();
    outb(PS2_DATA, cmd);
}

static uint8_t aux_read_ack(void) {
    wait_out_full();
    return inb(PS2_DATA);
}

/* ---- packet reassembly --------------------------------------------------- */

static void enqueue_event(int dx, int dy, int dz, uint8_t buttons) {
    uint8_t changed = last_buttons ^ buttons;
    mouse_event_t *e = &evq[evq_head];
    e->x = abs_x; e->y = abs_y;
    e->dx = dx; e->dy = dy; e->dz = dz;
    e->buttons = buttons;
    e->changed = changed;
    e->down = (uint8_t)(buttons & changed);
    uint32_t n = (evq_head + 1) % EVQ_SZ;
    if (n != evq_tail) evq_head = n;    /* drop oldest on overflow */
    last_buttons = buttons;
}

static void assemble_packet(void) {
    uint8_t b0 = pkt[0];
    uint8_t buttons = b0 & 0x07;

    int dx = pkt[1];
    if (b0 & 0x10) dx -= 256;           /* X sign extension */
    int dy = pkt[2];
    if (b0 & 0x20) dy -= 256;           /* Y sign extension */
    /* PS/2 Y grows upwards; the framebuffer Y grows downwards. */
    int sy = -dy;

    int dz = 0;
    if (wheel_mode && pkt_size == 4) {
        dz = pkt[3] & 0x0F;
        if (dz > 7) dz -= 16;           /* signed nibble */
    }

    abs_x += dx;
    abs_y += sy;
    if (abs_x < 0) abs_x = 0;
    if (abs_y < 0) abs_y = 0;
    if (abs_x > MOUSE_MAX_X) abs_x = MOUSE_MAX_X;
    if (abs_y > MOUSE_MAX_Y) abs_y = MOUSE_MAX_Y;

    pkt_count++;
    enqueue_event(dx, sy, dz, buttons);
}

uint32_t mouse_packet_count(void) { return pkt_count; }
uint32_t mouse_irq_count(void)    { return irq_count; }
uint32_t mouse_aux_bytes(void)    { return aux_bytes; }
uint32_t mouse_kbd_bytes_irq12(void) { return kbd_bytes_irq12; }

static void mouse_feed(uint8_t b) {
    /* Byte 0 sanity: bit3 must be 1.  A desynchronised stream resyncs on
     * the next plausible byte0 instead of sliding the packet window. */
    if (phase == 0) {
        if (!(b & 0x08)) return;
        pkt[0] = b; phase = 1; return;
    }
    pkt[phase] = b;
    phase++;
    if (phase >= pkt_size) {
        assemble_packet();
        phase = 0;
    }
}

/* ---- public driver API -------------------------------------------------- */

int mouse_poll_event(mouse_event_t *e) {
    if (evq_tail == evq_head) return 0;
    *e = evq[evq_tail];
    evq_tail = (evq_tail + 1) % EVQ_SZ;
    return 1;
}

void mouse_get_state(int *x, int *y, uint8_t *buttons) {
    if (x) *x = abs_x;
    if (y) *y = abs_y;
    if (buttons) *buttons = last_buttons;
}

void mouse_clamp(int px_w, int px_h) {
    if (px_w > 0 && abs_x >= px_w) abs_x = px_w - 1;
    if (px_h > 0 && abs_y >= px_h) abs_y = px_h - 1;
}

/* IRQ12 top half.  Both the kbd and aux ISRs route through ps2_drain();
 * this entry point exists so intr.c mirrors kb_irq() cleanly. */
void mouse_irq(void) {
    irq_count++;
    ps2_drain();
    console_wake_all();
}

/* Shared 8042 drain: status bit5 says whether the pending 0x60 byte is an
 * aux (mouse) byte or a keyboard scancode.  Routing here keeps the two
 * consumers from ever stealing each other's bytes. */
void ps2_drain(void) {
    int guard = 64;
    while ((inb(PS2_STATUS) & ST_OUT_FULL) && guard-- > 0) {
        uint8_t st = inb(PS2_STATUS);
        uint8_t b  = inb(PS2_DATA);
        if (st & ST_AUX_DATA) { aux_bytes++; mouse_feed(b); }
        else                   { kbd_bytes_irq12++; kbd_feed_byte(b); }
    }
}

void mouse_init(void) {
    evq_head = evq_tail = 0;
    last_buttons = 0;
    phase = 0;
    wheel_mode = 0;
    pkt_size = 3;

    /* 1. silence both ports, flush anything pending */
    wait_in_empty(); outb(PS2_CMD, 0xAD);   /* disable kbd */
    wait_in_empty(); outb(PS2_CMD, 0xA7);   /* disable aux */
    while (inb(PS2_STATUS) & ST_OUT_FULL) (void)inb(PS2_DATA);

    /* 2. config byte: enable IRQ12, keep aux port enabled */
    wait_in_empty(); outb(PS2_CMD, 0x20);
    wait_out_full();
    uint8_t cfg = inb(PS2_DATA);
    cfg |=  0x02;                           /* bit1: aux IRQ12 enable */
    cfg &= ~0x10;                           /* bit4: aux port clock enabled */
    wait_in_empty(); outb(PS2_CMD, 0x60);
    wait_in_empty(); outb(PS2_DATA, cfg);

    /* 3. enable the aux port */
    wait_in_empty(); outb(PS2_CMD, 0xA8);

    /* 4. IntelliMouse negotiation: the 200/100/80 sample-rate sequence
     * puts the device into wheel mode; 0xF2 identify returns 3 for a
     * wheel mouse.  A plain 3-byte mouse answers with ID 0 or 2 and we
     * stay on 3-byte packets. */
    aux_write(0xF3); aux_write(200); aux_read_ack();
    aux_write(0xF3); aux_write(100); aux_read_ack();
    aux_write(0xF3); aux_write(80);  aux_read_ack();
    aux_write(0xF2);                  /* identify */
    wait_out_full();
    uint8_t id = inb(PS2_DATA);
    if (id == 3) { wheel_mode = 1; pkt_size = 4; }

    /* 5. defaults, enable streaming */
    aux_write(0xF6); aux_read_ack();
    aux_write(0xF4); aux_read_ack();

    /* Re-enable the keyboard port (disabled in step 1); otherwise every
     * scancode stays gated behind the 8042 and the shell sees no input. */
    wait_in_empty(); outb(PS2_CMD, 0xAE);

    while (inb(PS2_STATUS) & ST_OUT_FULL) (void)inb(PS2_DATA);

    intr_unmask_irq(12);
}
