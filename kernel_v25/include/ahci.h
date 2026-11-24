#ifndef AHCI_H
#define AHCI_H

#include <stdint.h>

/*
 * ahci.h -- minimal SATA/AHCI storage driver for CatOS (D4).
 *
 * Enumerates a PCI class 0x01 / subclass 0x06 / prog-if 0x01 AHCI HBA,
 * maps the ABAR MMIO window, brings up the implemented ports, and serves
 * sector reads (IDENTIFY DEVICE + READ DMA EXT, LBA48).  It sits behind the
 * existing ATA PIO layer: ata_scan() appends AHCI ports to the same drive
 * table, so the FAT32/partition code keeps calling ata_read() unchanged.
 *
 * Scope / limits:
 *   - single HBA, one command slot per port, command-completion by polling
 *     the PxCI/PxSACT status bits (no MSI/X, no interrupt-driven ISR).
 *   - DMA descriptors and FIS area come from frame_alloc() at init (no large
 *     static .bss); the data buffer handed to ahci_read() must be an
 *     identity-mapped kernel buffer (physical == virtual), as the FS layer
 *     already provides.
 *   - WRITE DMA EXT is wired through the same command path but is exercised
 *     read-only by the probe; ATAPI/optical and port multiplier are out.
 */

#define AHCI_SECTOR_SIZE 512

/* Probe the AHCI HBA and initialise implemented ports.  Returns the number
 * of AHCI drives that answered IDENTIFY with a non-zero capacity. */
int       ahci_scan(void);
int       ahci_count(void);
uint32_t  ahci_sectors(int port);
const char *ahci_model_str(int port);

/* Read `count` sectors (max 128) at LBA48 `lba` into buf. 0 ok / -1 error. */
int  ahci_read(int port, uint32_t lba, uint8_t count, void *buf);
/* Write `count` sectors at LBA48 `lba` from buf. 0 ok / -1 error. */
int  ahci_write(int port, uint32_t lba, uint8_t count, const void *buf);

/* shell `ahci` diagnostic dump. */
void ahci_dump(void);

#endif /* AHCI_H */
