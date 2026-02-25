/*
 * tools/mkfs_aetherfs.c — AetherFS mkfs utility
 *
 * Creates a minimal EXT2 filesystem image compatible with the
 * AetherOS ext2 driver (fs/ext2.c).  Output is a flat binary
 * file that can be supplied as a virtual disk partition image,
 * loaded by diskman at boot, or embedded as an initrd.
 *
 * Build (host, no dependencies):
 *   gcc -O2 -o mkfs_aetherfs tools/mkfs_aetherfs.c
 *
 * Usage:
 *   ./mkfs_aetherfs <image.img> [size_mb]
 *
 * Defaults: 4 MB, 1 KB blocks, 512 inodes, single block group.
 *
 * Disk layout (1 KB blocks, single group):
 *   Block 0          : Boot block (zeroed, not in any group)
 *   Block 1          : Superblock  (s_first_data_block = 1)
 *   Block 2          : Group Descriptor Table
 *   Block 3          : Block Bitmap
 *   Block 4          : Inode Bitmap
 *   Blocks 5..68     : Inode Table  (512 inodes × 128 B = 64 blocks)
 *   Block 69         : Root directory data (inode 2)
 *   Blocks 70..N-1   : Free data blocks
 *
 * Bitmap index <-> physical block:
 *   bit i  =  block (first_data_block + i)  =  block (1 + i)
 *   So bit 0 = block 1, bit 68 = block 69, bit 69+ = free.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

/* =========================================================
 * On-disk structure definitions (match fs/ext2.c exactly)
 * ========================================================= */

#define EXT2_MAGIC          0xEF53u
#define EXT2_ROOT_INO       2u
#define EXT2_INODE_SIZE     128u   /* rev 0 fixed inode size */
#define EXT2_FT_DIR         2u
#define EXT2_S_IFDIR        0x4000u

#ifndef __attribute__
#define __attribute__(x)
#endif

#pragma pack(push, 1)

typedef struct {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count;
    uint32_t s_r_blocks_count;
    uint32_t s_free_blocks_count;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;
    uint32_t s_log_frag_size;
    uint32_t s_blocks_per_group;
    uint32_t s_frags_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;           /* 0xEF53 — at byte offset 56 within sb */
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    /* EXT2_DYNAMIC_REV */
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
    uint8_t  _reserved[760];    /* pad to exactly 1024 bytes */
} ext2_superblock_t;            /* sizeof == 1024 */

typedef struct {
    uint32_t bg_block_bitmap;
    uint32_t bg_inode_bitmap;
    uint32_t bg_inode_table;
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
    uint8_t  bg_reserved[12];   /* 32 bytes total */
} ext2_group_desc_t;

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
    uint32_t i_blocks;          /* count in 512-byte units */
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[15];       /* 12 direct + IND + DIND + TIND */
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_dir_acl;
    uint32_t i_faddr;
    uint8_t  i_osd2[12];        /* 128 bytes total */
} ext2_inode_t;

typedef struct {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
    char     name[255];
} ext2_dirent_t;

#pragma pack(pop)

/* =========================================================
 * Helpers
 * ========================================================= */

static void bm_set(uint8_t* bm, uint32_t bit)
{
    bm[bit >> 3] |= (uint8_t)(1u << (bit & 7u));
}

/* =========================================================
 * main
 * ========================================================= */
int main(int argc, char** argv)
{
    if (argc < 2) {
        fprintf(stderr,
            "Usage: %s <image.img> [size_mb]\n"
            "  size_mb : image size in megabytes (1–32, default 4)\n"
            "\nCreates an EXT2 filesystem image mountable by AetherOS diskman.\n",
            argv[0]);
        return 1;
    }

    const char* outfile  = argv[1];
    int         size_mb  = (argc >= 3) ? atoi(argv[2]) : 4;
    if (size_mb < 1)  size_mb = 1;
    if (size_mb > 32) size_mb = 32;   /* matches kernel's EXT2_MAX_SECTORS cap */

    /* ---- Derived constants ---- */
    const uint32_t BSIZE          = 1024u;
    const uint32_t TOTAL_BLOCKS   = (uint32_t)(size_mb * 1024);
    const uint32_t NUM_INODES     = 512u;
    const uint32_t INODE_TBL_BLKS = (NUM_INODES * EXT2_INODE_SIZE + BSIZE - 1)
                                     / BSIZE;   /* = 64 */

    /* Physical block numbers (block 0 = boot, block 1 = superblock) */
    const uint32_t BLK_SUPER    = 1u;
    const uint32_t BLK_GDT      = 2u;   /* first_data_block + 1 */
    const uint32_t BLK_BLKBM    = 3u;
    const uint32_t BLK_INOBM    = 4u;
    const uint32_t BLK_INOTAB   = 5u;
    const uint32_t BLK_ROOTDIR  = BLK_INOTAB + INODE_TBL_BLKS; /* = 69 */

    /* Counts */
    /* Block group covers blocks first_data_block..TOTAL_BLOCKS-1
     * (block 0 is the boot block, outside the group).           */
    const uint32_t BLOCKS_PER_GRP = TOTAL_BLOCKS - BLK_SUPER;
    const uint32_t USED_BMP_BITS  = (BLK_ROOTDIR - BLK_SUPER) + 1; /* metadata+rootdir */
    const uint32_t FREE_BLOCKS    = BLOCKS_PER_GRP - USED_BMP_BITS;
    const uint32_t FREE_INODES    = NUM_INODES - 2u;   /* inode 1 reserved, 2 = root */

    /* Sanity check: need at least one free data block */
    if (FREE_BLOCKS == 0) {
        fprintf(stderr, "Image too small to hold metadata.\n");
        return 1;
    }

    /* ---- Allocate zeroed image ---- */
    size_t  image_bytes = (size_t)TOTAL_BLOCKS * BSIZE;
    uint8_t* img = (uint8_t*)calloc(1, image_bytes);
    if (!img) { perror("calloc"); return 1; }

    uint32_t now = (uint32_t)time(NULL);

    /* ---- Verify struct size assumptions ---- */
    if (sizeof(ext2_superblock_t) != 1024) {
        fprintf(stderr, "Internal error: superblock struct is %zu bytes (expected 1024)\n",
                sizeof(ext2_superblock_t));
        free(img);
        return 1;
    }

    /* ================================================================
     * Block 1 — Superblock
     * ================================================================ */
    ext2_superblock_t* sb = (ext2_superblock_t*)(img + 1024u);
    sb->s_inodes_count       = NUM_INODES;
    sb->s_blocks_count       = TOTAL_BLOCKS;
    sb->s_r_blocks_count     = 0;
    sb->s_free_blocks_count  = FREE_BLOCKS;
    sb->s_free_inodes_count  = FREE_INODES;
    sb->s_first_data_block   = BLK_SUPER;       /* = 1 for 1KB blocks */
    sb->s_log_block_size     = 0;               /* 1024 << 0 = 1024 */
    sb->s_log_frag_size      = 0;
    sb->s_blocks_per_group   = BLOCKS_PER_GRP;
    sb->s_frags_per_group    = BLOCKS_PER_GRP;
    sb->s_inodes_per_group   = NUM_INODES;
    sb->s_mtime              = now;
    sb->s_wtime              = now;
    sb->s_mnt_count          = 0;
    sb->s_max_mnt_count      = 20;
    sb->s_magic              = EXT2_MAGIC;
    sb->s_state              = 1;   /* EXT2_VALID_FS */
    sb->s_errors             = 1;   /* EXT2_ERRORS_CONTINUE */
    sb->s_minor_rev_level    = 0;
    sb->s_lastcheck          = now;
    sb->s_checkinterval      = 0;
    sb->s_creator_os         = 5;   /* custom OS */
    sb->s_rev_level          = 0;   /* EXT2_GOOD_OLD_REV (128-byte inodes) */
    sb->s_def_resuid         = 0;
    sb->s_def_resgid         = 0;
    strncpy(sb->s_volume_name, "AetherFS", sizeof(sb->s_volume_name));

    /* ================================================================
     * Block 2 — Group Descriptor Table (single group)
     * ================================================================ */
    ext2_group_desc_t* gd = (ext2_group_desc_t*)(img + 2u * BSIZE);
    gd->bg_block_bitmap      = BLK_BLKBM;
    gd->bg_inode_bitmap      = BLK_INOBM;
    gd->bg_inode_table       = BLK_INOTAB;
    gd->bg_free_blocks_count = (uint16_t)FREE_BLOCKS;
    gd->bg_free_inodes_count = (uint16_t)FREE_INODES;
    gd->bg_used_dirs_count   = 1;   /* root directory */

    /* ================================================================
     * Block 3 — Block Bitmap
     * In the AetherOS driver: bit i = block (first_data_block + i).
     * So bit 0 = block 1 (superblock), bit (BLK_ROOTDIR-1) = root dir.
     * ================================================================ */
    uint8_t* blk_bm = img + BLK_BLKBM * BSIZE;
    for (uint32_t i = 0; i < USED_BMP_BITS; i++)
        bm_set(blk_bm, i);

    /* ================================================================
     * Block 4 — Inode Bitmap
     * bit 0 = inode 1 (reserved bad-blocks inode)
     * bit 1 = inode 2 (root directory)
     * ================================================================ */
    uint8_t* ino_bm = img + BLK_INOBM * BSIZE;
    bm_set(ino_bm, 0);   /* inode 1 */
    bm_set(ino_bm, 1);   /* inode 2 */

    /* ================================================================
     * Blocks 5–68 — Inode Table
     * Inode N is at:  inode_table_base + (N-1) * inode_size
     * Root inode = inode 2, at offset 1 * 128 = 128 within the table.
     * ================================================================ */
    ext2_inode_t* inode_tbl  = (ext2_inode_t*)(img + BLK_INOTAB * BSIZE);
    ext2_inode_t* root_inode = &inode_tbl[1];   /* index = inode_no - 1 = 1 */

    root_inode->i_mode        = (uint16_t)(EXT2_S_IFDIR | 0755u);
    root_inode->i_uid         = 0;
    root_inode->i_size        = BSIZE;          /* one 1024-byte block */
    root_inode->i_atime       = now;
    root_inode->i_ctime       = now;
    root_inode->i_mtime       = now;
    root_inode->i_dtime       = 0;
    root_inode->i_gid         = 0;
    root_inode->i_links_count = 2;              /* "." + parent ".." */
    root_inode->i_blocks      = BSIZE / 512u;   /* 2 × 512-byte units */
    root_inode->i_flags       = 0;
    root_inode->i_block[0]    = BLK_ROOTDIR;

    /* ================================================================
     * Block 69 — Root directory data
     * Two entries: "." and "..", both pointing to inode 2 (root).
     * rec_len of ".." fills the rest of the block.
     * ================================================================ */
    uint8_t* root_blk = img + BLK_ROOTDIR * BSIZE;

    /* Entry: "." */
    ext2_dirent_t* de = (ext2_dirent_t*)root_blk;
    de->inode     = EXT2_ROOT_INO;
    de->rec_len   = 12;                         /* min: 8 header + 4 aligned */
    de->name_len  = 1;
    de->file_type = EXT2_FT_DIR;
    de->name[0]   = '.';

    /* Entry: ".." — spans rest of block */
    de = (ext2_dirent_t*)(root_blk + 12u);
    de->inode     = EXT2_ROOT_INO;
    de->rec_len   = (uint16_t)(BSIZE - 12u);    /* fills remainder */
    de->name_len  = 2;
    de->file_type = EXT2_FT_DIR;
    de->name[0]   = '.';
    de->name[1]   = '.';

    /* ================================================================
     * Write image to disk
     * ================================================================ */
    FILE* fp = fopen(outfile, "wb");
    if (!fp) {
        perror(outfile);
        free(img);
        return 1;
    }
    size_t written = fwrite(img, 1, image_bytes, fp);
    fclose(fp);
    free(img);

    if (written != image_bytes) {
        fprintf(stderr, "mkfs_aetherfs: short write (%zu of %zu bytes)\n",
                written, image_bytes);
        return 1;
    }

    printf("mkfs_aetherfs: created '%s'\n", outfile);
    printf("  size          : %d MB  (%u × 1 KB blocks)\n", size_mb, TOTAL_BLOCKS);
    printf("  inodes        : %u  (free: %u)\n", NUM_INODES, FREE_INODES);
    printf("  free blocks   : %u\n", FREE_BLOCKS);
    printf("  superblock    : byte 1024  (block 1)\n");
    printf("  GDT           : block 2\n");
    printf("  block bitmap  : block %u\n", BLK_BLKBM);
    printf("  inode bitmap  : block %u\n", BLK_INOBM);
    printf("  inode table   : blocks %u–%u  (%u blocks)\n",
           BLK_INOTAB, BLK_INOTAB + INODE_TBL_BLKS - 1u, INODE_TBL_BLKS);
    printf("  root dir      : block %u  (inode 2)\n", BLK_ROOTDIR);
    printf("  data start    : block %u\n", BLK_ROOTDIR + 1u);
    return 0;
}
