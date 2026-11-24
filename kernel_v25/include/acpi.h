#ifndef ACPI_H
#define ACPI_H

/*
 * acpi.h -- minimal ACPI table discovery for CatOS platform bring-up.
 *
 * power.c already owns the FADT PM1_CNT power-off path; this module adds the
 * read-only table walk the SMP / APIC / AHCI lines need:
 *
 *   RSDP (EBDA, then 0xE0000..0xFFFFF) -> RSDT/XSDT -> FACP / APIC (MADT)
 *
 * MADT entries parsed: Processor Local APIC (type 0), IO APIC (type 1),
 * Interrupt Source Override (type 2).  Every physical pointer is bounds
 * checked against the identity-mapped RAM window before dereference; a
 * bogus firmware table is ignored, never faulted into.
 *
 * This is NOT an AML interpreter: only the fixed ACPI tables are decoded.
 */

#include <stdint.h>

#define ACPI_MAX_CPU    16
#define ACPI_MAX_IOAPIC 4
#define ACPI_MAX_ISO    16

typedef struct {
    uint8_t  proc_id;
    uint8_t  apic_id;
    uint32_t flags;        /* bit 0 = enabled */
} acpi_cpu_t;

typedef struct {
    uint8_t  ioapic_id;
    uint32_t addr;
    uint32_t irq_base;
} acpi_ioapic_t;

typedef struct {
    uint8_t  bus;
    uint8_t  source_irq;
    uint32_t global_irq;
    uint16_t flags;
} acpi_iso_t;

int            acpi_init(void);                 /* 1 = RSDP+FADT found */
int            acpi_present(void);
int            acpi_cpu_count(void);
const acpi_cpu_t *acpi_cpu(int i);
int            acpi_ioapic_count(void);
const acpi_ioapic_t *acpi_ioapic(int i);
int            acpi_iso_count(void);
const acpi_iso_t *acpi_iso(int i);
uint32_t       acpi_madt_lapic_addr(void);
uint32_t       acpi_rsdp_phys(void);
uint32_t       acpi_fadt_phys(void);

/* FADT reset register (GAS).  Returns 1 when a pure I/O-space reset block
 * is described; port and value are filled in that case. */
int            acpi_reset_block(uint16_t *port, uint8_t *value);

void           acpi_dump(void);                 /* shell `acpi` diagnostic */

#endif /* ACPI_H */
