#ifndef POWER_H
#define POWER_H

/*
 * power.h -- machine power-off (v2.67).
 *
 * The v2.61 `shutdown` did a protected-mode INT 15h / AX=5001h "APM
 * power-off".  That is wrong twice over:
 *
 *   1. APM 1.x is a 16-bit BIOS interface.  Calling it from protected
 *      mode with the kernel's own IDT gate does not "power off" -- it
 *      dispatches to whatever vector 0x15 happened to hold, which is an
 *      exception stub, so the machine took a fault and CatOS showed a
 *      BSOD.  That is the "shutdown always bluescreens" report.
 *   2. Even where APM happens to be present, modern firmware (QEMU,
 *      VMware, UEFI machines) has no APM BIOS at all.
 *
 * This module replaces it with the interfaces that actually exist:
 *
 *   A) ACPI: walk RSDP -> RSDT/XSDT -> FACP (FADT), read PM1a_CNT_BLK
 *      (FADT+0x48) and PM1b_CNT_BLK (+0x4C), then write SLP_TYPa<<10|SLP_EN
 *      to both.  SLP_TYPa is read from the FADT's own DSDT via the \_S5
 *      package when we can decode it; otherwise the standardized value
 *      (5) is used, then 0..7 are tried in order.  Every physical address
 *      is bounds-checked against the identity-mapped RAM window first --
 *      a table pointer outside that window is ignored rather than
 *      #PF'ing the kernel.
 *
 *   B) Known ACPI-less power ports, in the order QEMU/Bochs/VBox expect
 *      them (0x604, 0xB004, 0x4004).
 *
 *   C) 8042 reset (reboot) as the documented last resort -- reported
 *      honestly as "reboot, not power-off".
 *
 * Nothing here touches INT 15h, so a shutdown can no longer panic.
 */

int  power_shutdown(void);      /* 0 = the write was issued, -1 = unsupported */
int  power_shutdown_ports(void); /* legacy fixed-port fallback */
const char *power_last_method(void);
int  power_has_acpi(void);

#endif /* POWER_H */
