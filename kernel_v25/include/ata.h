#ifndef ATA_H
#define ATA_H

#include <stdint.h>

/*
 * CatOS ATA PIO driver (primary channel, LBA28).
 * Ports: 0x1F0-0x1F7 data/regs, 0x3F6 control.
 * Works with QEMU's PIIX IDE (-drive file=...,format=raw,if=ide).
 */

#define ATA_SECTOR_SIZE 512

/* Read `count` sectors (max 128) starting at LBA into buf. 0 = ok, -1 = error. */
int ata_read_sectors(uint32_t lba, uint8_t count, void *buf);
/* Write `count` sectors starting at LBA from buf. 0 = ok, -1 = error. */
int ata_write_sectors(uint32_t lba, uint8_t count, const void *buf);
/* IDENTIFY DEVICE: fill the drive's total sector count (LBA28, falls back
 * to LBA48).  Returns 0 on success, -1 if no drive / error. */
int ata_identify(uint32_t *sectors);

/* cached IDENTIFY info for hw.c (valid after the first successful
 * ata_identify; strings are empty before that) */
const char *ata_model_str(void);
const char *ata_serial_str(void);
const char *ata_fw_str(void);
int         ata_lba48_sup(void);

#endif
