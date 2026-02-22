/*
 * include/fs/fat32.h - FAT32 filesystem driver
 *
 * Implements read/write access to FAT32 volumes stored on ATA/SATA drives.
 * Integrates with the VFS layer so that FAT32 partitions can be mounted at
 * any path (e.g. "/", "/mnt/disk0").
 *
 * On-disk layout (standard FAT32):
 *
 *   Sector 0       : Boot sector (BPB)
 *   Sector 1       : FSInfo sector
 *   Sector 6       : Backup boot sector
 *   Sectors X..Y   : FAT region  (2 copies)
 *   Sectors Y+1..N : Data region (clusters 2 onward)
 *
 * Design choices:
 *   - LFN (Long File Name) support for names up to 255 chars
 *   - Write-back cluster cache (4 clusters per mount)
 *   - Journaling NOT present (FAT32 is inherently non-journaling)
 */
#ifndef FS_FAT32_H
#define FS_FAT32_H

#include <types.h>
#include <fs/vfs.h>

/* ── BPB (BIOS Parameter Block) as on disk ───────────────────────────── */
typedef struct {
    uint8_t  jmp_boot[3];
    uint8_t  oem_name[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint16_t root_entry_count;    /* 0 for FAT32 */
    uint16_t total_sectors_16;    /* 0 for FAT32 */
    uint8_t  media_type;
    uint16_t fat_size_16;         /* 0 for FAT32 */
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    /* FAT32 extended BPB */
    uint32_t fat_size_32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;        /* First cluster of root directory */
    uint16_t fs_info_sector;
    uint16_t backup_boot_sector;
    uint8_t  reserved[12];
    uint8_t  drive_number;
    uint8_t  reserved1;
    uint8_t  boot_signature;
    uint32_t volume_id;
    uint8_t  volume_label[11];
    uint8_t  fs_type[8];          /* "FAT32   " */
} PACKED fat32_bpb_t;

/* ── 8.3 Directory entry (32 bytes) ─────────────────────────────────── */
typedef struct {
    uint8_t  name[8];
    uint8_t  ext[3];
    uint8_t  attr;
    uint8_t  nt_reserved;
    uint8_t  create_time_tenth;
    uint16_t create_time;
    uint16_t create_date;
    uint16_t access_date;
    uint16_t cluster_hi;          /* High 16 bits of first cluster */
    uint16_t modify_time;
    uint16_t modify_date;
    uint16_t cluster_lo;          /* Low 16 bits of first cluster */
    uint32_t file_size;
} PACKED fat32_dirent_t;

/* ── LFN entry ───────────────────────────────────────────────────────── */
typedef struct {
    uint8_t  order;
    uint16_t name1[5];    /* UCS-2 chars 1-5 */
    uint8_t  attr;        /* Always 0x0F */
    uint8_t  type;
    uint8_t  checksum;
    uint16_t name2[6];    /* UCS-2 chars 6-11 */
    uint16_t cluster;     /* Always 0 */
    uint16_t name3[2];    /* UCS-2 chars 12-13 */
} PACKED fat32_lfn_t;

/* Attribute bits */
#define FAT_ATTR_READONLY  0x01
#define FAT_ATTR_HIDDEN    0x02
#define FAT_ATTR_SYSTEM    0x04
#define FAT_ATTR_VOLUME_ID 0x08
#define FAT_ATTR_DIR       0x10
#define FAT_ATTR_ARCHIVE   0x20
#define FAT_ATTR_LFN       0x0F   /* Long filename entry */

/* Special cluster values */
#define FAT32_FREE      0x00000000u
#define FAT32_EOC_MIN   0x0FFFFFF8u  /* End of chain */
#define FAT32_EOC       0x0FFFFFFFu
#define FAT32_BAD       0x0FFFFFF7u
#define FAT32_MASK      0x0FFFFFFFu

/* ── FSInfo sector ───────────────────────────────────────────────────── */
typedef struct {
    uint32_t lead_sig;     /* 0x41615252 */
    uint8_t  reserved1[480];
    uint32_t struct_sig;   /* 0x61417272 */
    uint32_t free_count;
    uint32_t next_free;
    uint8_t  reserved2[12];
    uint32_t trail_sig;    /* 0xAA550000 */
} PACKED fat32_fsinfo_t;

/* ── Mount context ───────────────────────────────────────────────────── */
#define FAT32_SECTOR_SIZE 512

typedef struct {
    bool          valid;
    fat32_bpb_t   bpb;

    /* Derived fields (computed from BPB at mount time) */
    uint32_t fat_lba;          /* LBA of FAT region start */
    uint32_t data_lba;         /* LBA of cluster 2 */
    uint32_t root_cluster;
    uint32_t sectors_per_cluster;
    uint32_t bytes_per_cluster;
    uint32_t total_clusters;
    uint32_t free_clusters;    /* From FSInfo */
    uint32_t next_free_hint;   /* From FSInfo */

    /* ATA device number (0 = primary master) */
    int      dev;
    int      partition_start_lba;

    /* 512-byte sector buffer for FAT I/O */
    uint8_t  sector_buf[FAT32_SECTOR_SIZE];
    uint32_t sector_buf_lba;   /* Which LBA is in the buffer */
    bool     sector_buf_dirty;

    char     mount_point[256]; /* VFS path this volume is mounted at */
    vfs_ops_t ops;             /* VFS ops filled by fat32_init */
} fat32_fs_t;

/* ── Public API ──────────────────────────────────────────────────────── */

/* Mount a FAT32 volume from ATA device @dev, partition starting at @lba_start */
int  fat32_init(fat32_fs_t* fs, int dev, int lba_start, const char* mount_point);

/* VFS operation implementations */
vfs_node_t* fat32_open(fat32_fs_t* fs, const char* path, int flags);
int         fat32_close(vfs_node_t* node);
ssize_t     fat32_read(vfs_node_t* node, uint64_t offset, size_t len, void* buf);
ssize_t     fat32_write(vfs_node_t* node, uint64_t offset, size_t len, const void* buf);
int         fat32_readdir(vfs_node_t* dir, uint32_t idx, vfs_dirent_t* out);
int         fat32_mkdir(fat32_fs_t* fs, const char* path);
int         fat32_unlink(fat32_fs_t* fs, const char* path);
int         fat32_stat(vfs_node_t* node, vfs_stat_t* st);

/* Low-level cluster helpers (exposed for fsck / disk tools) */
uint32_t fat32_next_cluster(fat32_fs_t* fs, uint32_t cluster);
uint32_t fat32_alloc_cluster(fat32_fs_t* fs, uint32_t prev);
int      fat32_free_chain(fat32_fs_t* fs, uint32_t start);
int      fat32_read_cluster(fat32_fs_t* fs, uint32_t cluster, void* buf);
int      fat32_write_cluster(fat32_fs_t* fs, uint32_t cluster, const void* buf);

/* Flush dirty buffers */
int  fat32_sync(fat32_fs_t* fs);
void fat32_unmount(fat32_fs_t* fs);

/* Global mount table (up to 4 FAT32 volumes) */
#define FAT32_MAX_MOUNTS 4
extern fat32_fs_t fat32_mounts[FAT32_MAX_MOUNTS];
int fat32_mount_auto(void);   /* Auto-detect and mount first FAT32 partition */
int fat32_mount(int drive, uint64_t lba_start, const char* mount_point);

#endif /* FS_FAT32_H */
