/*
 * include/fs/ext2.h - EXT2 filesystem driver (read/write)
 *
 * Parses an EXT2 image (ramdisk or ATA block device) and mounts it
 * as a VFS subtree.  Supports full read/write access: file creation,
 * writes, directory creation, and unlinking.  All mutations are applied
 * directly to the in-memory image (fs->data pointer).
 *
 * EXT2 layout (block size B, typically 1024 or 4096):
 *   Block 0          : boot sector (unused by us)
 *   Byte offset 1024 : superblock (always 1024 bytes, regardless of B)
 *   Block group 0    : block bitmap, inode bitmap, inode table, data blocks
 *   Block group 1+   : repeat
 *
 * References:
 *   https://www.nongnu.org/ext2-doc/ext2.html
 *   Linux kernel fs/ext2/
 */
#ifndef FS_EXT2_H
#define FS_EXT2_H

#include <types.h>
#include <fs/vfs.h>

/* ---- EXT2 on-disk structures (all little-endian) ---- */

/* Superblock — always located at byte offset 1024 from start of volume */
typedef struct {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count;
    uint32_t s_r_blocks_count;
    uint32_t s_free_blocks_count;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;  /* 0 for block size > 1024, else 1 */
    uint32_t s_log_block_size;    /* block_size = 1024 << s_log_block_size */
    uint32_t s_log_frag_size;
    uint32_t s_blocks_per_group;
    uint32_t s_frags_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;             /* 0xEF53 */
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;         /* 0=good-old, 1=dynamic */
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    /* EXT2_DYNAMIC_REV fields */
    uint32_t s_first_ino;
    uint16_t s_inode_size;
    uint16_t s_block_group_nr;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
    uint8_t  s_uuid[16];
    char     s_volume_name[16];
    char     s_last_mounted[64];
    uint32_t s_algo_bitmap;
    uint8_t  s_prealloc_blocks;
    uint8_t  s_prealloc_dir_blocks;
    uint16_t _alignment;
    uint8_t  s_journal_uuid[16];
    uint32_t s_journal_inum;
    uint32_t s_journal_dev;
    uint32_t s_last_orphan;
    uint32_t s_hash_seed[4];
    uint8_t  s_def_hash_version;
    uint8_t  _padding[3];
    uint32_t s_default_mount_opts;
    uint32_t s_first_meta_bg;
    uint8_t  _reserved[760];
} PACKED ext2_superblock_t;

#define EXT2_MAGIC 0xEF53

/* Block group descriptor */
typedef struct {
    uint32_t bg_block_bitmap;
    uint32_t bg_inode_bitmap;
    uint32_t bg_inode_table;
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
    uint8_t  bg_reserved[12];
} PACKED ext2_group_desc_t;

/* Inode */
#define EXT2_NDIR_BLOCKS 12
#define EXT2_IND_BLOCK   12
#define EXT2_DIND_BLOCK  13
#define EXT2_TIND_BLOCK  14
#define EXT2_N_BLOCKS    15

typedef struct {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks;   /* 512-byte blocks */
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[EXT2_N_BLOCKS];
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_dir_acl;
    uint32_t i_faddr;
    uint8_t  i_osd2[12];
} PACKED ext2_inode_t;

/* Inode mode bits */
#define EXT2_S_IFMT   0xF000
#define EXT2_S_IFREG  0x8000
#define EXT2_S_IFDIR  0x4000
#define EXT2_S_IFLNK  0xA000

/* Directory entry */
typedef struct {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
    char     name[255];
} PACKED ext2_dirent_t;

/* file_type values */
#define EXT2_FT_UNKNOWN  0
#define EXT2_FT_REG_FILE 1
#define EXT2_FT_DIR      2
#define EXT2_FT_CHRDEV   3
#define EXT2_FT_BLKDEV   4
#define EXT2_FT_FIFO     5
#define EXT2_FT_SOCK     6
#define EXT2_FT_SYMLINK  7

/* ---- Driver state ---- */
typedef struct {
    uint8_t*          data;           /* Pointer to raw image in memory     */
    uint64_t          size;           /* Size of image in bytes              */
    uint32_t          block_size;
    uint32_t          inodes_per_group;
    uint32_t          inode_size;
    uint32_t          first_data_block;
    uint32_t          group_count;
    ext2_superblock_t sb;
    /* Backing-device info for write-back persistence */
    int               backing_drive;  /* ATA drive index, -1 = no backing   */
    uint64_t          backing_lba;    /* First sector of this partition      */
    bool              dirty;          /* True if image modified since load   */
} ext2_fs_t;

/* ---- API ---- */

/*
 * Initialize an EXT2 filesystem from a memory-mapped image.
 * data: pointer to the raw disk image bytes
 * size: total size of the image in bytes
 * Returns: VFS root node of the EXT2 volume, or NULL on error.
 */
vfs_node_t* ext2_mount(uint8_t* data, uint64_t size);

/*
 * Register the EXT2 volume and mount it at a given VFS path.
 * drive_idx / lba_start identify the backing ATA partition for write-back
 * (-1 / 0 means no backing device, changes are RAM-only).
 * Returns 0 on success.
 */
int ext2_init(uint8_t* data, uint64_t size, const char* mount_point,
              int drive_idx, uint64_t lba_start);

/*
 * Write back any dirty ext2 images to their backing ATA devices.
 * Called by diskman_shutdown() before power-off.
 */
void ext2_flush_all(void);

#endif /* FS_EXT2_H */
