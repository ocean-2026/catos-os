/* usb_uhci.c -- USB 1.x host controller driver (UHCI) + HID keyboard.
 *
 * v2.26 -- requirement 5 of the 2026-11-14 overnight batch.  Brings CatOS
 * to modern-OS territory: USB keyboard input, alongside the PS/2 path.
 *
 * Hardware model (QEMU 6.2, `-usb -usbdevice keyboard`):
 *   PIIX3 UHCI PCI function 8086:7020 (also 7112 PIIX4), I/O BAR at BAR4.
 *   The emulated keyboard is a LOW-SPEED device at address 0, endpoint 1,
 *   interrupt IN, 8-byte HID boot-protocol report.
 *
 * Design notes:
 *   - Polling, not interrupts: the kernel timer (hw.c) calls usb_poll()
 *     every ~10ms and we inspect the transfer descriptor.  Keeps the IRQ
 *     dispatch path untouched (append-only intrusion into intr.c avoided)
 *     and is plenty for typing latency (<1 frame = 1ms transfer window).
 *   - No descriptor enumeration / SET_ADDRESS: a single QEMU device on
 *     the default address 0 is fully usable with boot protocol reports.
 *     Adding hub/address management later only extends usb_reset_port().
 *   - Memory: frame list (4 KiB, 4 KiB-aligned) + QH + TD + report buffer
 *     live in a fixed identity-mapped physical region (USB_DMA_PHYS),
 *     same technique as the audio DMA block (es1371.c).
 *
 * Reference: Intel UHCI 1.1 spec + OSDev wiki (TD layout verified 2026-11).
 */
#include "usb.h"
#include "io.h"
#include "keyboard.h"

extern volatile uint32_t timer_ticks;   /* intr.c IRQ0 counter, 100 Hz */

/* per-driver PCI config access (es1371.c pattern; kernel has no libc) */
static uint32_t pci_cfg_read(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) |
                    ((uint32_t)(dev & 0x1F) << 11) | ((uint32_t)(fn & 7) << 8) |
                    (uint32_t)(off & 0xFC);
    outl(0xCF8, addr);
    return inl(0xCFC);
}
static void pci_cfg_write(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off,
                          uint32_t v) {
    uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) |
                    ((uint32_t)(dev & 0x1F) << 11) | ((uint32_t)(fn & 7) << 8) |
                    (uint32_t)(off & 0xFC);
    outl(0xCF8, addr);
    outl(0xCFC, v);
}

/* tick-based sleep (timer is 100 Hz): ~10 ms granularity */
static void usb_delay_ticks(uint32_t ticks) {
    uint32_t t0 = timer_ticks;
    while ((uint32_t)(timer_ticks - t0) < ticks) { }
}

/* ---- fixed identity-mapped working area ------------------------------- */
/* 0x560000: 4 KiB frame list + 4 KiB structures + 4 KiB report buffers.
 * Clashes checked: ES1371_DMA_PHYS 0x550000, SND_DMA_PHYS 0x520000. */
#define USB_AREA_PHYS   0x560000u
#define USB_FRAME_LIST  (USB_AREA_PHYS)             /* 1024 * 4B = 4 KiB */
#define USB_QH_PHYS     (USB_AREA_PHYS + 0x1000u)   /* queue head       */
#define USB_TD_PHYS     (USB_AREA_PHYS + 0x1010u)   /* transfer desc    */
#define USB_BUF_PHYS    (USB_AREA_PHYS + 0x1020u)   /* 8B HID report    */
#define USB_CBUF_PHYS   (USB_AREA_PHYS + 0x1030u)   /* 32B control data */
#define USB_CTDA_PHYS   (USB_AREA_PHYS + 0x1060u)   /* control TD #0    */
#define USB_CTDB_PHYS   (USB_AREA_PHYS + 0x1080u)   /* control TD #1    */
#define USB_CTDC_PHYS   (USB_AREA_PHYS + 0x10A0u)   /* control TD #2    */
/* pointers into the identity mapping */
#define FR ((volatile uint32_t *)USB_FRAME_LIST)
#define QH ((volatile uint32_t *)USB_QH_PHYS)
#define TD ((volatile uint32_t *)USB_TD_PHYS)
#define RP ((volatile uint8_t  *)USB_BUF_PHYS)
#define CB ((volatile uint8_t  *)USB_CBUF_PHYS)
#define CT0 ((volatile uint32_t *)USB_CTDA_PHYS)
#define CT1 ((volatile uint32_t *)USB_CTDB_PHYS)
#define CT2 ((volatile uint32_t *)USB_CTDC_PHYS)

/* ---- PCI -------------------------------------------------------------- */
#define USB_VENDOR_INTEL    0x8086u
#define USB_DEV_PIIX3_UHCI  0x7020u
#define USB_DEV_PIIX4_UHCI  0x7112u
#define PCI_COMMAND_IO      0x0001u
#define PCI_COMMAND_MASTER  0x0004u

/* ---- UHCI I/O register offsets (BAR4 + off) --------------------------- */
#define R_USBCMD    0x00
#define R_USBSTS    0x02
#define R_USBINTR   0x04
#define R_FRNUM     0x06
#define R_FRBASEADD 0x08
#define R_SOFMOD    0x0C
#define R_PORTSC(n) (0x10 + 2 * ((n) - 1))

#define CMD_RS      0x0001u     /* run/stop                          */
#define CMD_HCRESET 0x0002u     /* self-clearing                     */
#define CMD_GRESET  0x0004u     /* global reset (usb lines)          */
#define CMD_CF      0x0040u     /* configure flag                    */
#define CMD_MAXP    0x0080u     /* 64-byte max packet                */

#define STS_HCHALT  0x0020u

#define PSC_CONNECT   0x0001u   /* RO: device attached               */
#define PSC_CONNCHG   0x0002u   /* R/WC: connect status changed      */
#define PSC_ENABLE    0x0004u   /* R/W: port enable                  */
#define PSC_ENCHG     0x0008u   /* R/WC: enable changed              */
#define PSC_LOWSPEED  0x0100u   /* RO: low speed device              */
#define PSC_RESET     0x0200u   /* R/W: port reset                   */
#define PSC_ALWAYS1   0x0080u   /* RO bit7 always reads 1 (valid)    */

/* ---- TD fields -------------------------------------------------------- */
#define TD_TERM     0x00000001u
#define TD_DEPTH    0x00000004u
#define TD_STATUS_LS      (1u << 26)
#define TD_STATUS_IOC     (1u << 24)
#define TD_STATUS_ACTIVE  (1u << 23)
#define TD_STATUS_ERR_MSK (0x1Fu << 17)   /* stall/dbuffer/babble/NAK/CRC */
#define TD_STATUS_LEN_MSK 0x000007FFu
#define TD_PID_IN   0x69u
#define TD_EP_MSK   (0x0Fu << 15)
#define TD_DEV_MSK  (0x7Fu << 8)
#define TD_TOGG_MSK (1u << 19)
#define TD_MAXLEN(n) (((uint32_t)((n) - 1u) & 0x7FFu) << 21)

/* ---- module state ------------------------------------------------------ */
static uint16_t uio;            /* I/O base (BAR4)                 */
static int      probed;         /* controller found                */
static int      running;        /* schedule live                   */
static int      kbd_port;       /* 1 or 2, 0 = none                */
static int      kbd_ls;         /* low speed device                */
static uint32_t poll_ticks;     /* last tick we polled at          */
static uint32_t prev_mods;      /* previous report modifier byte   */
static uint8_t  prev_keys[6];   /* previous report keycode slots   */
static int      td_armed;      /* interrupt TD handed to the HC    */
static uint32_t toggle;         /* HID data toggle                 */
static uint32_t dev_addr;       /* 0 during enumeration, 1 after   */
static int      enumerated;     /* SET_CONFIGURATION accepted      */
static uint16_t usb_io_dbg;     /* breadcrumb: probed io base      */
static uint32_t reports;        /* completed reports (diagnostic)  */
static uint32_t errors;         /* TD error completions            */
static uint32_t polls;          /* poll invocations                */

static uint16_t rd(uint32_t off)   { return inw(uio + off); }
static void     wr(uint32_t off, uint16_t v) { outw(uio + off, v); }

int usb_ready(void)    { return running && kbd_port; }
int usb_kb_port(void)  { return kbd_port; }
uint32_t usb_reports(void) { return reports; }
uint32_t usb_errors(void)  { return errors; }

/* ---- probe ------------------------------------------------------------ */
int usb_probe(void) {
    if (probed) return 1;
    for (uint8_t dev = 0; dev < 32; dev++)
    for (uint8_t fn = 0; fn < 8; fn++) {
        uint32_t id = pci_cfg_read(0, dev, fn, 0);
        uint16_t v  = (uint16_t)(id & 0xFFFFu);
        if (v != USB_VENDOR_INTEL) continue;
        uint16_t d = (uint16_t)(id >> 16);
        if (d != USB_DEV_PIIX3_UHCI && d != USB_DEV_PIIX4_UHCI) continue;

        uint32_t bar4 = pci_cfg_read(0, dev, fn, 0x20);
        if (!(bar4 & 1u)) continue;              /* I/O BAR only */
        uio = (uint16_t)(bar4 & 0xFFFCu);
        uint16_t cmd = (uint16_t)pci_cfg_read(0, dev, fn, 0x04);
        cmd |= PCI_COMMAND_IO | PCI_COMMAND_MASTER;
        pci_cfg_write(0, dev, fn, 0x04, cmd);
        /* NOTE: no legacy-USB handoff write here -- on QEMU's PIIX3 the
         * 0xC0 register is NOT the PIIX4 legacy register and writing it
         * broke IRQ0/IRQ1 delivery.  QEMU has no BIOS USB emulation. */
        probed = 1;
        usb_io_dbg = uio;
        return 1;
    }
    return 0;
}

/* ---- reset controller, build schedule, start -------------------------- */
static void uhci_reset_hc(void) {
    wr(R_USBCMD, CMD_HCRESET);
    for (volatile int i = 0; i < 100000; i++) { }
    int guard = 0;
    while ((rd(R_USBCMD) & CMD_HCRESET) && guard++ < 1000)
        for (volatile int i = 0; i < 1000; i++) { }
    wr(R_USBCMD, CMD_GRESET);
    usb_delay_ticks(1);                   /* >= 10 ms at 100 Hz */
    wr(R_USBCMD, 0);
}

static void build_schedule(void) {
    /* one queue head executed every frame; the vertical pointer is
     * re-aimed at whichever transfer chain is currently active */
    QH[0] = TD_TERM;                                  /* horizontal: none */
    QH[1] = TD_TERM;                                  /* vertical: idle   */
    for (int i = 0; i < 1024; i++)
        FR[i] = USB_QH_PHYS | (1u << 1);              /* type = QH        */
}

static void kbd_requeue(void) {
    /* The HC ADVANCES the QH element pointer as it completes TDs (it
     * writes our TD[0] -- TERM -- into QH[1]), so every re-arm must also
     * re-aim the queue head at the interrupt TD. */
    QH[1] = USB_TD_PHYS;
    TD[0] = TD_TERM;                                  /* next: terminate  */
    TD[1] = (kbd_ls ? TD_STATUS_LS : 0u) | TD_STATUS_ACTIVE;
    TD[2] = (TD_PID_IN)
          | (dev_addr << 8)
          | (1u << 15)                                /* endpoint 1       */
          | (toggle << 19)
          | TD_MAXLEN(8);
    TD[3] = USB_BUF_PHYS;
    td_armed = 1;
}


/* ---- control transfer engine ------------------------------------------
 * Runs a SETUP(+data)(+status) chain through the same QH, depth-first.
 * Called before the interrupt schedule is armed, so we own the QH. */
static void ctrl_td(volatile uint32_t *td, uint32_t next, uint8_t pid,
                    uint8_t addr, uint8_t ep, uint32_t tgl, uint32_t len,
                    uint32_t buf) {
    td[0] = next;
    td[1] = (kbd_ls ? TD_STATUS_LS : 0u) | TD_STATUS_ACTIVE;
    td[2] = pid
          | ((uint32_t)addr << 8)
          | ((uint32_t)ep << 15)
          | (tgl << 19)
          | (len == 0 ? 0x7FFu << 21 : TD_MAXLEN(len == 0 ? 2048 : len));
    /* zero-length packets use MaxLength=0x7FF (2048) by convention */
    if (len == 0) td[2] = pid | ((uint32_t)addr << 8) | ((uint32_t)ep << 15)
                       | (tgl << 19) | (0x7FFu << 21);
    else td[2] = pid | ((uint32_t)addr << 8) | ((uint32_t)ep << 15)
               | (tgl << 19) | TD_MAXLEN(len);
    td[3] = buf;
}

static int ctrl_wait(volatile uint32_t *last) {
    for (int g = 0; g < 200; g++) {           /* up to ~2 s at 10ms steps */
        uint32_t st = last[1];
        if (!(st & TD_STATUS_ACTIVE)) {
            if (st & TD_STATUS_ERR_MSK) return -1;
            return 0;
        }
            usb_delay_ticks(1);
    }
    return -1;
}

/* setup: 8-byte request; datalen 0 = no data stage */
static int usb_control(const volatile uint8_t *req, int datalen) {
    for (int i = 0; i < 8; i++) CB[i] = req[i];

    /* SETUP (toggle 0) */
    ctrl_td(CT0, USB_CTDB_PHYS | TD_DEPTH, 0x2Du, (uint8_t)dev_addr, 0, 0, 8,
            USB_CBUF_PHYS);
    if (datalen > 0) {
        /* IN data (toggle 1) + OUT zero-length status (toggle 1) */
        ctrl_td(CT1, USB_CTDC_PHYS | TD_DEPTH, 0x69u, (uint8_t)dev_addr, 0, 1,
                datalen, USB_CBUF_PHYS + 8);
        ctrl_td(CT2, TD_TERM, 0xE1u, (uint8_t)dev_addr, 0, 1, 0, 0);
        QH[1] = USB_CTDA_PHYS;
        if (ctrl_wait(CT2) != 0) return -1;
    } else {
        /* IN zero-length status only */
        ctrl_td(CT1, TD_TERM, 0x69u, (uint8_t)dev_addr, 0, 1, 0, 0);
        QH[1] = USB_CTDA_PHYS;
        if (ctrl_wait(CT1) != 0) return -1;
    }
    return 0;
}

/* full enumeration of the (single) device on the attached port */
static int usb_enumerate(void) {
    static const uint8_t GETDEV[8] = { 0x80, 0x06, 0x00, 0x01, 0x00, 0x00,
                                       0x08, 0x00 };
    static const uint8_t SETCFG[8]  = { 0x00, 0x09, 0x01, 0x00, 0x00, 0x00,
                                        0x00, 0x00 };
    uint8_t addr1[8] = { 0x00, 0x05, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00 };

    if (usb_control(GETDEV, 8) != 0) return -1;

    /* SET_ADDRESS(1) goes to the default address 0; only afterwards do
     * we move the driver-side dev_addr to 1. */
    if (usb_control(addr1, 0) != 0) return -1;
    dev_addr = 1;
    usb_delay_ticks(2);                        /* set-address recovery */

    if (usb_control(SETCFG, 0) != 0) return -1;
    enumerated = 1;
    return 0;
}

int usb_init(void) {
    if (!usb_probe()) return 0;

    uhci_reset_hc();

    build_schedule();
    for (int i = 0; i < 1024; i++) FR[i] = USB_QH_PHYS | (1u << 1);
    outl(uio + R_FRBASEADD, USB_FRAME_LIST);   /* 32-bit register! */
    wr(R_SOFMOD, 0x40);                               /* 1 ms frames   */
    wr(R_USBINTR, 0);                                 /* polling mode  */
    wr(R_FRNUM, 0);

    running = 1;
    wr(R_USBCMD, CMD_RS | CMD_MAXP | CMD_CF);
    return 1;
}

/* ---- device attach: reset+enable a port, learn speed ------------------ */
static int usb_reset_port(int port) {
    uint16_t v = rd(R_PORTSC(port));
    if (!(v & PSC_CONNECT)) return 0;
    wr(R_PORTSC(port), v & ~PSC_CONNCHG & ~PSC_ENCHG);  /* ack changes   */
    wr(R_PORTSC(port), PSC_RESET);                      /* drive reset   */
    usb_delay_ticks(10);                                /* 100 ms        */
    wr(R_PORTSC(port), 0);                              /* release reset */
    usb_delay_ticks(5);                                 /* 50 ms         */
    wr(R_PORTSC(port), PSC_ENABLE);
    int guard = 0;
    while (guard++ < 30) {
        if (rd(R_PORTSC(port)) & PSC_ENABLE) break;
        usb_delay_ticks(1);
    }
    if (!(rd(R_PORTSC(port)) & PSC_ENABLE)) return 0;
    kbd_ls = (rd(R_PORTSC(port)) & PSC_LOWSPEED) ? 1 : 0;
    return 1;
}

/* ---- HID -> i8042 scancode set 1 --------------------------------------
 * Translating to set-1 scancodes (instead of touching the decoder) lets
 * the entire existing keyboard stack -- shift/caps/ctrl state, E0-prefixed
 * keys, scrollback keys, IME -- work unchanged for USB input. */
static uint8_t hid2set1(uint8_t k) {
    /* letters */
    if (k >= 0x04 && k <= 0x1D) {
        /* set-1 make codes are NOT row-contiguous; explicit per-letter */
        static const uint8_t L[26] = {
            0x1E,0x30,0x2E,0x20,0x12,0x21,0x22,0x23,0x17,0x24, /* a..j */
            0x25,0x26,0x32,0x31,0x18,0x19,0x10,0x13,0x1F,0x14, /* k..t */
            0x16,0x2F,0x11,0x2D,0x15,0x2C };                   /* u..z */
        return L[k - 0x04];
    }
    /* digits row: 1..9,0 */
    if (k >= 0x1E && k <= 0x27) {
        static const uint8_t D[10] =
            { 0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B };
        return D[k - 0x1E];
    }
    switch (k) {
    case 0x28: return 0x1C;   /* enter      */
    case 0x29: return 0x01;   /* escape     */
    case 0x2A: return 0x0E;   /* backspace  */
    case 0x2B: return 0x0F;   /* tab        */
    case 0x2C: return 0x39;   /* space      */
    case 0x2D: return 0x0C;   /* - _        */
    case 0x2E: return 0x0D;   /* = +        */
    case 0x2F: return 0x1A;   /* [ {        */
    case 0x30: return 0x1B;   /* ] }        */
    case 0x31: return 0x2B;   /* \ |        */
    case 0x33: return 0x27;   /* ; :        */
    case 0x34: return 0x28;   /* ' "        */
    case 0x35: return 0x29;   /* ` ~        */
    case 0x36: return 0x33;   /* , <        */
    case 0x37: return 0x34;   /* . >        */
    case 0x38: return 0x35;   /* / ?        */
    case 0x39: return 0x3A;   /* caps lock  */
    case 0x3A: return 0x3B;   /* f1  */  case 0x3B: return 0x3C;
    case 0x3C: return 0x3D;   /* f3  */  case 0x3D: return 0x3E;
    case 0x3E: return 0x3F;   /* f5  */  case 0x3F: return 0x40;
    case 0x40: return 0x41;   /* f7  */  case 0x41: return 0x42;
    case 0x42: return 0x43;   /* f9  */  case 0x43: return 0x44;   /* f10 */
    case 0x44: return 0x57;   /* f11 */  case 0x45: return 0x58;   /* f12 */
    case 0x4A: return 0x47;   /* home   */
    case 0x4B: return 0x49;   /* pgup   */
    case 0x4C: return 0x53;   /* delete */
    case 0x4D: return 0x4F;   /* end    */
    case 0x4E: return 0x51;   /* pgdn   */
    case 0x4F: return 0x4D;   /* right  */
    case 0x50: return 0x4B;   /* left   */
    case 0x51: return 0x50;   /* down   */
    case 0x52: return 0x48;   /* up     */
    case 0x53: return 0x45;   /* numlock */
    case 0x65: return 0x5C;   /* application (menu) */
    }
    return 0;
}

/* modifier byte transitions -> synthetic set-1 shift/ctrl/alt make/break */
static void feed_mods(uint32_t now) {
    static const struct { uint32_t mask; uint8_t make, brk; } M[] = {
        { 0x02, 0x2A, 0xAA },   /* left shift  */
        { 0x20, 0x36, 0xB6 },   /* right shift */
        { 0x01, 0x1D, 0x9D },   /* left ctrl   */
        { 0x10, 0x1D, 0x9D },   /* right ctrl  */
        { 0x04, 0x38, 0xB8 },   /* left alt    */
        { 0x40, 0x38, 0xB8 },   /* right alt   */
    };
    for (unsigned i = 0; i < sizeof(M) / sizeof(M[0]); i++) {
        uint32_t was = prev_mods & M[i].mask;
        uint32_t cur = now & M[i].mask;
        if (was && !cur) kb_usb_feed(M[i].brk);
        else if (!was && cur) kb_usb_feed(M[i].make);
    }
}

static void feed_report(const volatile uint8_t *r) {
    uint32_t mods = r[0] & 0x77;          /* gui bits (0x08,0x80) ignored */
    feed_mods(mods);
    prev_mods = mods;
    /* slot compare: newly pressed -> make, gone -> break */
    for (int i = 2; i < 8; i++) {
        uint8_t k = r[i];
        if (k && k < 0x80) {
            int was = 0;
            for (int j = 0; j < 6; j++) if (prev_keys[j] == k) was = 1;
            if (!was) {
                uint8_t sc = hid2set1(k);
                if (sc) kb_usb_feed(sc);
            }
        }
    }
    for (int j = 0; j < 6; j++) {
        uint8_t old = prev_keys[j];
        if (old) {
            int still = 0;
            for (int i = 2; i < 8; i++) if (r[i] == old) still = 1;
            if (!still) {
                uint8_t sc = hid2set1(old);
                if (sc) kb_usb_feed(sc | 0x80);
            }
        }
    }
    for (int j = 0; j < 6; j++) prev_keys[j] = 0;
    for (int i = 2; i < 8 && i - 2 < 6; i++)
        if (r[i] && r[i] < 0x80) prev_keys[i - 2] = r[i];
}

/* ---- periodic poll (hw.c timer) --------------------------------------- */
void usb_poll(void) {
    if (!running || !kbd_port) return;
    uint32_t now = timer_ticks;
    if (now - poll_ticks < 10) return;        /* ~100 Hz tick -> 10ms */
    poll_ticks = now;
    polls++;

    if (!td_armed) return;                    /* TD not in schedule   */
    uint32_t st = TD[1];
    if (st & TD_STATUS_ACTIVE) return;        /* transfer in flight   */
    td_armed = 0;

    if (st & TD_STATUS_ERR_MSK) { errors++; toggle ^= 1; kbd_requeue(); return; }

    uint32_t len = (st & TD_STATUS_LEN_MSK) + 1;
    if (len > 8) len = 8;
    if (len >= 1) {
        reports++;
        feed_report(RP);
    }
    toggle ^= 1;
    kbd_requeue();
}

/* ---- device hot-attach scan (boot + `usb` command) --------------------- */
int usb_scan_devices(void) {
    if (!running) return 0;
    for (int port = 1; port <= 2; port++) {
        uint16_t v = rd(R_PORTSC(port));
        if (v == 0xFFFFu) continue;           /* no such port         */
        if (!(v & PSC_ALWAYS1)) continue;     /* not a port register  */
        if (v & PSC_CONNCHG) {
            wr(R_PORTSC(port), v & ~PSC_CONNCHG);  /* ack in own write */
            v = rd(R_PORTSC(port));
        }
        if (v & PSC_CONNECT) {
            if (usb_reset_port(port)) {
                kbd_port = port;
                toggle = 0;
                prev_mods = 0;
                for (int j = 0; j < 6; j++) prev_keys[j] = 0;
                if (usb_enumerate() != 0) { kbd_port = 0; continue; }
                QH[1] = USB_TD_PHYS;          /* vertical -> interrupt TD */
                kbd_requeue();
                return 1;
            }
        }
    }
    return 0;
}

/* ---- boot wiring ------------------------------------------------------- */
int usb_start(void) {
    if (!usb_init()) return 0;
    usb_scan_devices();
    return 1;
}

const char *usb_status(char *buf, int cap) {
    int o = 0;
    const char *s = probed ? "uhci@io" : "no controller";
    for (; *s && o < cap - 1; s++) buf[o++] = *s;
    if (probed) {
        for (const char *q = " kb:"; *q && o < cap - 1; q++) buf[o++] = *q;
        buf[o++] = kbd_port ? (char)('0' + kbd_port) : '-';
        for (const char *q = kbd_ls ? " ls" : " fs"; *q && o < cap - 1; q++)
            buf[o++] = *q;
        for (const char *q = " reports="; *q && o < cap - 1; q++) buf[o++] = *q;
        { uint32_t x = reports; char d[12]; int n = 0;
          do { d[n++] = (char)('0' + x % 10); x /= 10; } while (x);
          while (n && o < cap - 1) buf[o++] = d[--n]; }
        for (const char *q = " errors="; *q && o < cap - 1; q++) buf[o++] = *q;
        { uint32_t x = errors; char d[12]; int n = 0;
          do { d[n++] = (char)('0' + x % 10); x /= 10; } while (x);
          while (n && o < cap - 1) buf[o++] = d[--n]; }
    }
    buf[o] = 0;
    return buf;
}
