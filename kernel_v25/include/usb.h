/* usb.h -- v2.26 USB 1.x host controller (UHCI) + HID keyboard. */
#ifndef USB_H
#define USB_H

#include <stdint.h>

int      usb_probe(void);        /* PCI scan 8086:7020/7112 (PIIX3/4 UHCI) */
int      usb_init(void);         /* reset HC, build 1ms QH/TD schedule, run */
int      usb_start(void);        /* init + hot-attach scan; boot entry     */
int      usb_scan_devices(void); /* rescan ports, attach keyboard, arm TD  */
void     usb_poll(void);         /* periodic (timer tick): reap/requeue TD */

int      usb_ready(void);
int      usb_kb_port(void);      /* 1/2, 0 = none attached                 */
uint32_t usb_reports(void);      /* completed HID reports                  */
uint32_t usb_errors(void);       /* TD error completions                   */
const char *usb_status(char *buf, int cap);

#endif /* USB_H */
