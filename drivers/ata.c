/*
 * drivers/ata.c - ATA/IDE disk driver (PIO mode)
 *
 * Supports up to 4 drives across primary and secondary ATA buses.
 * Uses polling (PIO) mode for simplicity. DMA can be added later.
 *
 * Follows the ATA-6 specification for LBA28 and LBA48 addressing.
 */
#include <drivers/ata.h>
#include <kernel.h>
#include <types.h>
#include <string.h>

/* Global drive table */
ata_drive_t ata_drives[4];
int ata_drive_count = 0;

/* ============================================================
 * Low-level PIO helpers
 * ============================================================ */

/* Wait for BSY to clear (up to 1000 iterations) */
static int ata_wait_ready(ata_drive_t* drive)
{
    for (int i = 0; i < 100000; i++) {
        uint8_t status = inb(drive->base + ATA_REG_STATUS);
        if (!(status & ATA_SR_BSY)) return 0;
        io_wait();
    }
    return -1; /* Timeout */
}

/* Wait for DRQ (data request) to set */
static int ata_wait_drq(ata_drive_t* drive)
{
    for (int i = 0; i < 100000; i++) {
        uint8_t status = inb(drive->base + ATA_REG_STATUS);
        if (status & ATA_SR_ERR)  return -1;
        if (status & ATA_SR_DRQ)  return 0;
        io_wait();
    }
    return -1; /* Timeout */
}

/* 400ns delay: read alt-status 4 times */
static void ata_delay400(ata_drive_t* drive)
{
    for (int i = 0; i < 4; i++) {
        inb(drive->ctrl);
    }
}

/* ============================================================
 * IDENTIFY command
 * ============================================================ */

void ata_identify(ata_drive_t* drive)
{
    if (!drive->present) return;

    /* Select drive */
    outb(drive->base + ATA_REG_HDDEVSEL,
         0xA0 | (drive->drive << 4));
    ata_delay400(drive);

    /* Zero out sector count and LBA registers */
    outb(drive->base + ATA_REG_SECCOUNT, 0);
    outb(drive->base + ATA_REG_LBA_LOW,  0);
    outb(drive->base + ATA_REG_LBA_MID,  0);
    outb(drive->base + ATA_REG_LBA_HIGH, 0);

    /* Send IDENTIFY command */
    outb(drive->base + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
    ata_delay400(drive);

    /* Check if drive exists */
    uint8_t status = inb(drive->base + ATA_REG_STATUS);
    if (status == 0) {
        drive->present = false;
        return;
    }

    /* Wait for BSY to clear */
    if (ata_wait_ready(drive) < 0) {
        drive->present = false;
        return;
    }

    /* Check for ATAPI (non-ATA device) */
    uint8_t cl = inb(drive->base + ATA_REG_LBA_MID);
    uint8_t ch = inb(drive->base + ATA_REG_LBA_HIGH);
    if (cl != 0 || ch != 0) {
        drive->present = false; /* ATAPI device, not supported here */
        return;
    }

    if (ata_wait_drq(drive) < 0) {
        drive->present = false;
        return;
    }

    /* Read 256 words of IDENTIFY data */
    uint16_t id[256];
    for (int i = 0; i < 256; i++) {
        id[i] = inw(drive->base + ATA_REG_DATA);
    }

    /* Parse model string (bytes 54-93, big-endian word pairs) */
    for (int i = 0; i < 20; i++) {
        drive->model[i * 2]     = (char)(id[27 + i] >> 8);
        drive->model[i * 2 + 1] = (char)(id[27 + i] & 0xFF);
    }
    drive->model[40] = '\0';

    /* Strip trailing spaces from model */
    for (int i = 39; i >= 0 && drive->model[i] == ' '; i--) {
        drive->model[i] = '\0';
    }

    /* Check LBA48 support (word 83, bit 10) */
    drive->lba48 = (id[83] & (1 << 10)) != 0;

    /* Get sector count */
    if (drive->lba48) {
        drive->sectors = ((uint64_t)id[103] << 48) |
                         ((uint64_t)id[102] << 32) |
                         ((uint64_t)id[101] << 16) |
                          (uint64_t)id[100];
    } else {
        drive->sectors = ((uint32_t)id[61] << 16) | id[60];
    }
}

/* ============================================================
 * Read / Write (LBA48 PIO)
 * ============================================================ */

/*
 * ata_read - read @count sectors from @lba into @buf.
 * @count: 1-256 sectors per call
 * Returns 0 on success, -1 on error.
 */
int ata_read(ata_drive_t* drive, uint64_t lba, uint16_t count, void* buf)
{
    if (!drive->present) return -1;
    if (count == 0 || count > 256) return -1;

    ata_wait_ready(drive);

    if (drive->lba48) {
        /* LBA48 extended addressing */
        outb(drive->base + ATA_REG_HDDEVSEL, 0x40 | (drive->drive << 4));
        outb(drive->base + ATA_REG_SECCOUNT, (uint8_t)((count >> 8) & 0xFF));
        outb(drive->base + ATA_REG_LBA_LOW,  (uint8_t)((lba >> 24) & 0xFF));
        outb(drive->base + ATA_REG_LBA_MID,  (uint8_t)((lba >> 32) & 0xFF));
        outb(drive->base + ATA_REG_LBA_HIGH, (uint8_t)((lba >> 40) & 0xFF));
        outb(drive->base + ATA_REG_SECCOUNT, (uint8_t)(count & 0xFF));
        outb(drive->base + ATA_REG_LBA_LOW,  (uint8_t)(lba & 0xFF));
        outb(drive->base + ATA_REG_LBA_MID,  (uint8_t)((lba >> 8) & 0xFF));
        outb(drive->base + ATA_REG_LBA_HIGH, (uint8_t)((lba >> 16) & 0xFF));
        outb(drive->base + ATA_REG_COMMAND,  ATA_CMD_READ_PIO_EXT);
    } else {
        /* LBA28 */
        outb(drive->base + ATA_REG_HDDEVSEL,
             0xE0 | (drive->drive << 4) | ((lba >> 24) & 0x0F));
        outb(drive->base + ATA_REG_SECCOUNT, (uint8_t)count);
        outb(drive->base + ATA_REG_LBA_LOW,  (uint8_t)(lba & 0xFF));
        outb(drive->base + ATA_REG_LBA_MID,  (uint8_t)((lba >> 8) & 0xFF));
        outb(drive->base + ATA_REG_LBA_HIGH, (uint8_t)((lba >> 16) & 0xFF));
        outb(drive->base + ATA_REG_COMMAND,  ATA_CMD_READ_PIO);
    }

    uint16_t* ptr = (uint16_t*)buf;
    for (uint16_t s = 0; s < count; s++) {
        ata_delay400(drive);

        if (ata_wait_drq(drive) < 0) return -1;

        /* Read 256 words (512 bytes) per sector */
        for (int w = 0; w < 256; w++) {
            ptr[s * 256 + w] = inw(drive->base + ATA_REG_DATA);
        }
    }

    return 0;
}

/*
 * ata_write - write @count sectors from @buf to @lba.
 */
int ata_write(ata_drive_t* drive, uint64_t lba, uint16_t count, const void* buf)
{
    if (!drive->present) return -1;
    if (count == 0 || count > 256) return -1;

    ata_wait_ready(drive);

    if (drive->lba48) {
        outb(drive->base + ATA_REG_HDDEVSEL, 0x40 | (drive->drive << 4));
        outb(drive->base + ATA_REG_SECCOUNT, (uint8_t)((count >> 8) & 0xFF));
        outb(drive->base + ATA_REG_LBA_LOW,  (uint8_t)((lba >> 24) & 0xFF));
        outb(drive->base + ATA_REG_LBA_MID,  (uint8_t)((lba >> 32) & 0xFF));
        outb(drive->base + ATA_REG_LBA_HIGH, (uint8_t)((lba >> 40) & 0xFF));
        outb(drive->base + ATA_REG_SECCOUNT, (uint8_t)(count & 0xFF));
        outb(drive->base + ATA_REG_LBA_LOW,  (uint8_t)(lba & 0xFF));
        outb(drive->base + ATA_REG_LBA_MID,  (uint8_t)((lba >> 8) & 0xFF));
        outb(drive->base + ATA_REG_LBA_HIGH, (uint8_t)((lba >> 16) & 0xFF));
        outb(drive->base + ATA_REG_COMMAND,  ATA_CMD_WRITE_PIO_EXT);
    } else {
        outb(drive->base + ATA_REG_HDDEVSEL,
             0xE0 | (drive->drive << 4) | ((lba >> 24) & 0x0F));
        outb(drive->base + ATA_REG_SECCOUNT, (uint8_t)count);
        outb(drive->base + ATA_REG_LBA_LOW,  (uint8_t)(lba & 0xFF));
        outb(drive->base + ATA_REG_LBA_MID,  (uint8_t)((lba >> 8) & 0xFF));
        outb(drive->base + ATA_REG_LBA_HIGH, (uint8_t)((lba >> 16) & 0xFF));
        outb(drive->base + ATA_REG_COMMAND,  ATA_CMD_WRITE_PIO);
    }

    const uint16_t* ptr = (const uint16_t*)buf;
    for (uint16_t s = 0; s < count; s++) {
        ata_delay400(drive);

        if (ata_wait_drq(drive) < 0) return -1;

        for (int w = 0; w < 256; w++) {
            outw(drive->base + ATA_REG_DATA, ptr[s * 256 + w]);
        }
    }

    /* Flush write cache */
    outb(drive->base + ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH);
    ata_wait_ready(drive);

    return 0;
}

/* ============================================================
 * Initialization: detect drives on both buses
 * ============================================================ */

static void ata_probe_drive(int bus_idx, int drive_idx,
                             uint16_t base, uint16_t ctrl)
{
    ata_drive_t* d = &ata_drives[ata_drive_count];

    d->base   = base;
    d->ctrl   = ctrl;
    d->drive  = (uint8_t)drive_idx;
    d->lba48  = false;
    d->sectors = 0;
    d->present = false;

    /* Select drive */
    outb(base + ATA_REG_HDDEVSEL, 0xA0 | (drive_idx << 4));
    io_wait();

    /* Check if drive responds */
    outb(base + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
    io_wait();

    uint8_t status = inb(base + ATA_REG_STATUS);
    if (status == 0) return; /* No drive */

    d->present = true;
    ata_identify(d);

    if (d->present) {
        kinfo("ATA%d: %s%s - %llu sectors (%llu MB)",
              ata_drive_count,
              d->model,
              d->lba48 ? " [LBA48]" : " [LBA28]",
              d->sectors,
              d->sectors * ATA_SECTOR_SIZE / (1024 * 1024));
        ata_drive_count++;
    }
}

void ata_init(void)
{
    ata_drive_count = 0;

    /* Reset both channels */
    outb(ATA_PRIMARY_CTRL,   0x04); /* Assert SRST */
    io_wait();
    outb(ATA_PRIMARY_CTRL,   0x00); /* Clear SRST */
    outb(ATA_SECONDARY_CTRL, 0x04);
    io_wait();
    outb(ATA_SECONDARY_CTRL, 0x00);
    io_wait();

    /* Probe all 4 possible drives */
    ata_probe_drive(0, ATA_MASTER, ATA_PRIMARY_BASE,   ATA_PRIMARY_CTRL);
    ata_probe_drive(0, ATA_SLAVE,  ATA_PRIMARY_BASE,   ATA_PRIMARY_CTRL);
    ata_probe_drive(1, ATA_MASTER, ATA_SECONDARY_BASE, ATA_SECONDARY_CTRL);
    ata_probe_drive(1, ATA_SLAVE,  ATA_SECONDARY_BASE, ATA_SECONDARY_CTRL);

    kinfo("ATA: detected %d drive(s)", ata_drive_count);
}

ata_drive_t* ata_get_drive(int bus, int drive)
{
    int idx = bus * 2 + drive;
    if (idx < 0 || idx >= 4) return NULL;
    if (!ata_drives[idx].present) return NULL;
    return &ata_drives[idx];
}

/* Compatibility wrappers used by fat32.c and blockcache.c */
int ata_read_sectors(ata_drive_t* drive, uint64_t lba, uint16_t count, void* buf)
{
    return ata_read(drive, lba, count, buf);
}

int ata_write_sectors(ata_drive_t* drive, uint64_t lba, uint16_t count, const void* buf)
{
    return ata_write(drive, lba, count, buf);
}
