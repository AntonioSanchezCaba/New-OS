/*
 * include/kernel/diskman.h — Disk manager
 *
 * Scans ATA drives at boot, reads MBR partition tables, and attempts to
 * mount recognized filesystems (FAT32, ext2) into the VFS at /mnt/hd0, etc.
 */
#pragma once
#include <types.h>

#define DISKMAN_MAX_PARTS   16
#define DISKMAN_MAX_DRIVES   4

typedef struct {
    int      drive_idx;    /* ATA drive index (0-3)          */
    int      part_idx;     /* Partition index (0-3 for MBR)  */
    uint64_t lba_start;    /* First sector                   */
    uint64_t lba_size;     /* Sector count                   */
    uint8_t  type;         /* MBR partition type byte        */
    char     mount[32];    /* Mount point e.g. "/mnt/hd0p1"  */
    bool     mounted;
} disk_partition_t;

/* Scan all ATA drives and try to mount partitions into the VFS.
 * Called during kernel boot, after VFS is ready. */
void diskman_init(void);

/* Get partition table */
int                   diskman_count(void);
const disk_partition_t* diskman_get(int idx);

/* Unmount all (called on shutdown) */
void diskman_shutdown(void);
