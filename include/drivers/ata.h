/*
 * drivers/ata.h - ATA/IDE disk driver interface (PIO mode)
 *
 * Supports up to 4 drives: Primary Master/Slave, Secondary Master/Slave.
 */
#ifndef DRIVERS_ATA_H
#define DRIVERS_ATA_H

#include <types.h>

/* ATA I/O port bases */
#define ATA_PRIMARY_BASE    0x1F0
#define ATA_PRIMARY_CTRL    0x3F6
#define ATA_SECONDARY_BASE  0x170
#define ATA_SECONDARY_CTRL  0x376

/* ATA register offsets from base */
#define ATA_REG_DATA        0x00
#define ATA_REG_ERROR       0x01
#define ATA_REG_FEATURES    0x01
#define ATA_REG_SECCOUNT    0x02
#define ATA_REG_LBA_LOW     0x03
#define ATA_REG_LBA_MID     0x04
#define ATA_REG_LBA_HIGH    0x05
#define ATA_REG_HDDEVSEL    0x06
#define ATA_REG_STATUS      0x07
#define ATA_REG_COMMAND     0x07
#define ATA_REG_ALTSTATUS   0x00  /* Offset from control base */
#define ATA_REG_DEVCTRL     0x00

/* ATA status register bits */
#define ATA_SR_BSY   0x80  /* Busy */
#define ATA_SR_DRDY  0x40  /* Drive ready */
#define ATA_SR_DWF   0x20  /* Drive write fault */
#define ATA_SR_DSC   0x10  /* Drive seek complete */
#define ATA_SR_DRQ   0x08  /* Data request ready */
#define ATA_SR_CORR  0x04  /* Corrected data */
#define ATA_SR_IDX   0x02  /* Index */
#define ATA_SR_ERR   0x01  /* Error */

/* ATA commands */
#define ATA_CMD_READ_PIO          0x20
#define ATA_CMD_READ_PIO_EXT      0x24
#define ATA_CMD_WRITE_PIO         0x30
#define ATA_CMD_WRITE_PIO_EXT     0x34
#define ATA_CMD_CACHE_FLUSH       0xE7
#define ATA_CMD_CACHE_FLUSH_EXT   0xEA
#define ATA_CMD_PACKET            0xA0
#define ATA_CMD_IDENTIFY_PACKET   0xA1
#define ATA_CMD_IDENTIFY          0xEC

/* Drive selection */
#define ATA_MASTER  0x00
#define ATA_SLAVE   0x01

#define ATA_PRIMARY   0
#define ATA_SECONDARY 1

/* Sector size */
#define ATA_SECTOR_SIZE 512

/* Drive info structure */
typedef struct {
    uint16_t base;          /* I/O base port */
    uint16_t ctrl;          /* Control port */
    uint8_t  drive;         /* ATA_MASTER or ATA_SLAVE */
    bool     present;       /* Drive found? */
    bool     lba48;         /* LBA48 supported? */
    uint64_t sectors;       /* Total sector count */
    char     model[41];     /* Drive model string */
    char     serial[21];    /* Drive serial number */
} ata_drive_t;

/* ATA driver API */
void ata_init(void);
int  ata_read(ata_drive_t* drive, uint64_t lba, uint16_t count, void* buf);
int  ata_write(ata_drive_t* drive, uint64_t lba, uint16_t count, const void* buf);
void ata_identify(ata_drive_t* drive);
ata_drive_t* ata_get_drive(int bus, int drive);

/* Compatibility wrappers (used by fat32, blockcache) */
int ata_read_sectors(ata_drive_t* drive, uint64_t lba, uint16_t count, void* buf);
int ata_write_sectors(ata_drive_t* drive, uint64_t lba, uint16_t count, const void* buf);

/* Number of detected drives */
extern int ata_drive_count;
extern ata_drive_t ata_drives[4];

#endif /* DRIVERS_ATA_H */
