#ifndef SMP_H
#define SMP_H

/*
 * smp.h -- platform SMP diagnostics for CatOS.
 *
 * Status (deliberate): this kernel is still UP-scheduled.  This module
 * only *observes* the firmware topology discovered via ACPI MADT + CPUID
 * and the BSP's own LAPIC state.  It does NOT release application
 * processors (no INIT-SIPI-SIPI), does not program the IOAPIC redirection
 * table, and does not run a per-core scheduler.  AP bring-up and multi-core
 * task parallelism are a rewrite-level change (see docs: identity map /
 * per-core GDT-TSS-stack / preemptive scheduler) tracked separately.
 *
 * The `smp` shell command dumps: firmware-present CPUs, BSP APIC ID, each
 * MADT entry's state (BSP online / AP deferred), LAPIC/IOAPIC addresses,
 * and the UP scheduler's alive-task count and idle ticks.
 */

void smp_init(void);

int  smp_firmware_cpus(void);   /* CPUs listed by MADT (1 if no MADT)   */
int  smp_online_cpus(void);     /* CPUs this kernel is running on (= 1) */

void smp_dump(void);            /* shell `smp` diagnostic               */

#endif /* SMP_H */
