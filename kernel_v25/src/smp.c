/*
 * smp.c -- SMP topology observation + diagnostics (see include/smp.h).
 *
 * Read-only: MADT/CPUID enumeration and BSP LAPIC base/MSR inspection.
 * No AP startup, no IOAPIC reprogramming, no scheduler changes.
 */

#include "smp.h"
#include "acpi.h"
#include "task.h"
#include "vga.h"
#include "serial_log.h"
#include <stdint.h>

static int      fw_cpus;
static int      online_cpus;
static uint8_t  bsp_apic_id;
static uint32_t lapic_base;
static int      lapic_enabled;
static int      apic_feature;

static void cpuid1(uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile ("cpuid"
                      : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                      : "a"(1));
}

static uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

/* ---- minimal console printers ---------------------------------------- */
static void wh(uint32_t v) {
    static const char h[] = "0123456789abcdef";
    char b[11]; b[0]='0'; b[1]='x';
    for (int i = 0; i < 8; i++) b[2+i] = h[(v >> (28 - i*4)) & 0xF];
    b[10] = 0; vga_write(b);
}

static void wdec(uint32_t v) {
    char b[12]; int i = 11; b[i--] = 0;
    if (!v) { vga_write("0"); return; }
    while (v && i >= 0) { b[i--] = '0' + (v % 10); v /= 10; }
    vga_write(b + i + 1);
}

void smp_init(void) {
    acpi_init();

    uint32_t a, b, c, d;
    cpuid1(&a, &b, &c, &d);
    bsp_apic_id  = (uint8_t)(b >> 24);
    apic_feature = (d >> 9) & 1;

    /* IA32_APIC_BASE MSR (0x1B): base addr [12..31], bit 11 = enable. */
    uint64_t base = rdmsr(0x1B);
    lapic_base   = (uint32_t)(base & 0xFFFFF000u);
    lapic_enabled= (int)((base >> 11) & 1);

    int n = acpi_cpu_count();
    fw_cpus = n > 0 ? n : 1;
    online_cpus = 1;   /* UP scheduler: only the BSP runs tasks        */

    ser_write("[smp] fw_cpus="); ser_putc('0' + (fw_cpus % 10));
    ser_write(" bsp_apic="); ser_putc('0' + bsp_apic_id);
    ser_write(" lapic="); ser_hex("", lapic_base); ser_write("\n");
}

int smp_firmware_cpus(void) { return fw_cpus; }
int smp_online_cpus(void)  { return online_cpus; }

void smp_dump(void) {
    vga_write("smp: firmware CPUs="); wdec((uint32_t)fw_cpus);
    vga_write(" online="); wdec((uint32_t)online_cpus); vga_write("\n");

    vga_write("  BSP APIC ID="); wdec(bsp_apic_id);
    vga_write("  APIC feature="); vga_write(apic_feature ? "yes" : "no");
    vga_write("  LAPIC="); wh(lapic_base);
    vga_write(" enabled="); vga_write(lapic_enabled ? "yes" : "no");
    vga_write("\n");

    if (acpi_madt_lapic_addr()) {
        vga_write("  MADT LAPIC addr="); wh(acpi_madt_lapic_addr()); vga_write("\n");
    }
    for (int i = 0; i < acpi_ioapic_count(); i++) {
        const acpi_ioapic_t *io = acpi_ioapic(i);
        vga_write("  IOAPIC "); wdec(io->ioapic_id);
        vga_write(" addr="); wh(io->addr);
        vga_write(" base="); wdec(io->irq_base); vga_write("\n");
    }

    vga_write("  per-CPU state:\n");
    int n = acpi_cpu_count();
    if (n == 0) {
        vga_write("    CPU "); wdec(bsp_apic_id);
        vga_write("  online (BSP, no MADT entries)\n");
    }
    for (int i = 0; i < n; i++) {
        const acpi_cpu_t *cpu = acpi_cpu(i);
        vga_write("    CPU "); wdec(cpu->apic_id);
        vga_write("  ");
        if (cpu->apic_id == bsp_apic_id)
            vga_write("online (BSP)");
        else
            vga_write("deferred (AP not released; UP scheduler)");
        vga_write("\n");
    }

    vga_write("  scheduler: alive tasks="); wdec((uint32_t)task_count());
    vga_write("  idle ticks="); wdec(task_idle_ticks()); vga_write("\n");
}
