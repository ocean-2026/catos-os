#ifndef ATA_H
#define ATA_H

#include <stdint.h>

/*
 * CatOS ATA PIO driver (primary/secondary bus x master/slave, LBA28).
 * Ports: 0x1F0-0x1F7 data/regs, 0x3F6 control.
 * Works with QEMU's PIIX IDE (-drive file=...,format=raw,if=ide).
 *
 * P2: the driver now probes up to four IDE slots and keeps a per-drive
 * slot record.  ata_read()/ata_write() take an explicit drive index;
 * the legacy single-drive wrappers target drive 0.
 */

#define ATA_SECTOR_SIZE 512
#define ATA_DRIVES      4

/* Probe all four IDE slots; fill the internal drive table.  Returns the
 * number of drives that answered IDENTIFY with a non-zero capacity.
 * Safe to call once; repeated calls are no-ops. */
int  ata_scan(void);
/* Number of present drives (after ata_scan). */
int  ata_drive_count(void);
/* Total sectors reported by drive `d` (0 if absent). */
uint32_t ata_drive_sectors(int d);

/* Read `count` sectors (max 128) starting at LBA into buf. 0 = ok, -1 = error. */
int  ata_read(int d, uint32_t lba, uint8_t count, void *buf);
/* Write `count` sectors starting at LBA from buf. 0 = ok, -1 = error. */
int  ata_write(int d, uint32_t lba, uint8_t count, const void *buf);

/* ---- legacy single-drive API (drive 0) ---- */
int  ata_read_sectors(uint32_t lba, uint8_t count, void *buf);
int  ata_write_sectors(uint32_t lba, uint8_t count, const void *buf);
/* IDENTIFY DEVICE: fill drive 0's total sector count.  0 ok / -1 absent. */
int  ata_identify(uint32_t *sectors);

/* cached IDENTIFY info for hw.c (valid after drive 0 answered; strings
 * are empty before that) */
const char *ata_model_str(void);
const char *ata_serial_str(void);
const char *ata_fw_str(void);
int         ata_lba48_sup(void);

#endif
