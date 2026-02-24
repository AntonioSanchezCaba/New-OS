/*
 * kernel/diskman.c — Boot-time disk manager
 *
 * For each ATA drive found: reads MBR, parses partition table,
 * identifies filesystem type, mounts into VFS.
 *
 * MBR partition type codes:
 *   0x0B / 0x0C = FAT32
 *   0x83        = Linux (ext2/3/4)
 *   0x05 / 0x0F = Extended (ignored for now)
 */
#include <kernel/diskman.h>
#include <fs/blockcache.h>
#include <fs/fat32.h>
#include <fs/ext2.h>
#include <fs/vfs.h>
#include <drivers/ata.h>
#include <memory.h>
#include <string.h>
#include <kernel.h>
#include <types.h>

#pragma pack(push, 1)
typedef struct {
    uint8_t  status;
    uint8_t  chs_first[3];
    uint8_t  type;
    uint8_t  chs_last[3];
    uint32_t lba_start;
    uint32_t lba_size;
} mbr_part_t;

typedef struct {
    uint8_t   boot_code[446];
    mbr_part_t parts[4];
    uint16_t  signature;       /* 0xAA55 */
} mbr_t;
#pragma pack(pop)

static disk_partition_t g_parts[DISKMAN_MAX_PARTS];
static int              g_count = 0;

/* =========================================================
 * Mount point name builder
 * ========================================================= */
static void build_mount(char* out, int drive, int part)
{
    /* /mnt/hd0p1 ... /mnt/hd3p4 */
    out[0] = '/'; out[1] = 'm'; out[2] = 'n'; out[3] = 't'; out[4] = '/';
    out[5] = 'h'; out[6] = 'd'; out[7] = (char)('0' + drive);
    out[8] = 'p'; out[9] = (char)('1' + part); out[10] = '\0';
}

/* =========================================================
 * Try to mount a FAT32 partition
 * ========================================================= */
static bool try_mount_fat32(int drive_idx, uint64_t lba_start,
                             const char* mount_point)
{
    /* Ensure /mnt directory exists */
    vfs_mkdir("/mnt");

    int rc = fat32_mount(drive_idx, lba_start, mount_point);
    if (rc == 0) {
        kinfo("DISKMAN: FAT32 mounted %s", mount_point);
        return true;
    }
    return false;
}

/* =========================================================
 * Try to mount an EXT2 partition
 *
 * The ext2 driver works with a memory-mapped image, so we copy the
 * partition into a heap buffer (up to EXT2_MAX_SECTORS = 32 MB).
 * The buffer is kept alive indefinitely — the driver reads from it.
 * ========================================================= */
#define EXT2_MAX_SECTORS  65536ULL   /* 32 MB @ 512 B/sector */

static bool try_mount_ext2(int drive_idx, uint64_t lba_start, uint64_t lba_size,
                            const char* mount_point)
{
    vfs_mkdir("/mnt");

    /*
     * Quick-validate ext2 magic in the superblock (byte offset 1024,
     * field s_magic at offset +56 within the superblock = byte 1080).
     * Sector 2 (lba_start + 2) starts at byte 1024 when sector size is 512.
     */
    uint64_t sb_lba = lba_start + (1024 / BCACHE_SECTOR_SIZE);
    uint8_t* sb_sec = bcache_get(drive_idx, sb_lba);
    if (!sb_sec) return false;

    uint16_t magic;
    /* s_magic is at byte offset 56 within the superblock; superblock starts
     * at byte 1024; 1024 % 512 = 0, so we're at the start of sector sb_lba.
     * s_magic offset within this sector: (1024 % 512) + 56 = 56. */
    memcpy(&magic, sb_sec + (1024 % BCACHE_SECTOR_SIZE) + 56, sizeof(magic));
    if (magic != 0xEF53) {
        kdebug("DISKMAN: drive %d lba %llu not ext2 (magic=0x%04X)",
               drive_idx, lba_start, magic);
        return false;
    }

    /* Clamp to 32 MB */
    if (lba_size > EXT2_MAX_SECTORS) {
        kwarn("DISKMAN: ext2 at %s is %llu sectors, clamping to %llu (32 MB)",
              mount_point, lba_size, EXT2_MAX_SECTORS);
        lba_size = EXT2_MAX_SECTORS;
    }

    uint64_t image_bytes = lba_size * BCACHE_SECTOR_SIZE;
    uint8_t* image = (uint8_t*)kmalloc((size_t)image_bytes);
    if (!image) {
        kerror("DISKMAN: out of memory for ext2 image (%llu kB)",
               image_bytes / 1024);
        return false;
    }

    /* Copy partition sectors into image buffer */
    for (uint64_t i = 0; i < lba_size; i++) {
        uint8_t* sec = bcache_get(drive_idx, lba_start + i);
        if (sec)
            memcpy(image + i * BCACHE_SECTOR_SIZE, sec, BCACHE_SECTOR_SIZE);
        else
            memset(image + i * BCACHE_SECTOR_SIZE, 0,   BCACHE_SECTOR_SIZE);
    }

    int rc = ext2_init(image, image_bytes, mount_point);
    if (rc != 0) {
        kfree(image);
        kwarn("DISKMAN: ext2_init failed at %s (rc=%d)", mount_point, rc);
        return false;
    }

    kinfo("DISKMAN: ext2 mounted %s (%llu kB image)", mount_point, image_bytes / 1024);
    /* image intentionally kept alive — ext2 driver reads from it */
    return true;
}

/* =========================================================
 * Scan one drive
 * ========================================================= */
static void scan_drive(int drive_idx)
{
    extern ata_drive_t ata_drives[];
    if (!ata_drives[drive_idx].present) return;

    kinfo("DISKMAN: scanning drive %d (%s)",
          drive_idx, ata_drives[drive_idx].model);

    uint8_t* sector = bcache_get(drive_idx, 0);
    if (!sector) { klog_warn("DISKMAN: cannot read MBR on drive %d", drive_idx); return; }

    const mbr_t* mbr = (const mbr_t*)sector;
    if (mbr->signature != 0xAA55) {
        klog_warn("DISKMAN: drive %d has no valid MBR (sig=%04X)",
                  drive_idx, mbr->signature);
        return;
    }

    for (int pi = 0; pi < 4 && g_count < DISKMAN_MAX_PARTS; pi++) {
        const mbr_part_t* p = &mbr->parts[pi];
        if (p->type == 0 || p->lba_size == 0) continue;

        disk_partition_t* dp = &g_parts[g_count++];
        dp->drive_idx = drive_idx;
        dp->part_idx  = pi;
        dp->lba_start = p->lba_start;
        dp->lba_size  = p->lba_size;
        dp->type      = p->type;
        dp->mounted   = false;
        build_mount(dp->mount, drive_idx, pi);

        kinfo("DISKMAN: drive %d part %d type=0x%02X lba=%u size=%u",
              drive_idx, pi, p->type, p->lba_start, p->lba_size);

        /* Try to mount */
        if (p->type == 0x0B || p->type == 0x0C) {
            dp->mounted = try_mount_fat32(drive_idx, p->lba_start, dp->mount);
        } else if (p->type == 0x83) {
            dp->mounted = try_mount_ext2(drive_idx, p->lba_start,
                                         p->lba_size, dp->mount);
            if (!dp->mounted)
                kinfo("DISKMAN: ext2 at %s skipped (not ext2 or too large)",
                      dp->mount);
        }
    }
}

/* =========================================================
 * Public API
 * ========================================================= */
void diskman_init(void)
{
    g_count = 0;
    kinfo("DISKMAN: scanning ATA drives...");
    for (int d = 0; d < DISKMAN_MAX_DRIVES; d++)
        scan_drive(d);
    kinfo("DISKMAN: found %d partitions", g_count);
}

int diskman_count(void) { return g_count; }

const disk_partition_t* diskman_get(int idx)
{
    if (idx < 0 || idx >= g_count) return NULL;
    return &g_parts[idx];
}

void diskman_shutdown(void)
{
    bcache_flush_all();
    kinfo("DISKMAN: all caches flushed");
}
