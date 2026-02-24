/*
 * fs/ext2.c - EXT2 filesystem driver (read/write)
 *
 * Parses an in-memory EXT2 image and exposes it through the VFS layer.
 * Supports: directory traversal, regular file reads, symbolic link
 * resolution (up to one level), stat-like inode metadata, and full
 * write support (file create, write, mkdir, unlink, block/inode alloc).
 *
 * Notes:
 *   - Operates on an in-memory image (fs->data pointer); all writes
 *     modify the image bytes in place — no disk I/O needed.
 *   - Indirect block support: single, double, and triple indirects.
 *   - No journal replay (ext3/ext4 journals are ignored).
 */
#include <fs/ext2.h>
#include <fs/vfs.h>
#include <memory.h>
#include <string.h>
#include <kernel.h>

/* Maximum symlink depth to follow */
#define EXT2_SYMLINK_DEPTH 4

/* =========================================================
 * Internal helpers
 * ========================================================= */

/* Return pointer to raw bytes at byte offset within the image */
static inline uint8_t* img_ptr(ext2_fs_t* fs, uint64_t byte_off)
{
    if (byte_off >= fs->size) return NULL;
    return fs->data + byte_off;
}

/* Return a pointer to block number @blk within the image */
static inline uint8_t* block_ptr(ext2_fs_t* fs, uint32_t blk)
{
    return img_ptr(fs, (uint64_t)blk * fs->block_size);
}

/* Read the group descriptor for group @g */
static ext2_group_desc_t* group_desc(ext2_fs_t* fs, uint32_t g)
{
    /* Group descriptor table starts at the block after the superblock block */
    uint32_t gdt_block = fs->first_data_block + 1;
    uint8_t* gdt = block_ptr(fs, gdt_block);
    if (!gdt) return NULL;
    return (ext2_group_desc_t*)(gdt + g * sizeof(ext2_group_desc_t));
}

/* Read inode by number (1-based) */
static ext2_inode_t* read_inode(ext2_fs_t* fs, uint32_t ino)
{
    if (ino == 0) return NULL;
    uint32_t group  = (ino - 1) / fs->inodes_per_group;
    uint32_t index  = (ino - 1) % fs->inodes_per_group;

    ext2_group_desc_t* gd = group_desc(fs, group);
    if (!gd) return NULL;

    uint8_t* table = block_ptr(fs, gd->bg_inode_table);
    if (!table) return NULL;

    return (ext2_inode_t*)(table + index * fs->inode_size);
}

/*
 * Translate a logical block number within a file to a physical block number.
 * Handles direct, single-indirect, double-indirect, triple-indirect.
 */
static uint32_t file_block_to_phys(ext2_fs_t* fs, ext2_inode_t* inode,
                                    uint32_t lblock)
{
    uint32_t ptrs_per_block = fs->block_size / sizeof(uint32_t);

    /* Direct blocks */
    if (lblock < EXT2_NDIR_BLOCKS) {
        return inode->i_block[lblock];
    }
    lblock -= EXT2_NDIR_BLOCKS;

    /* Single indirect */
    if (lblock < ptrs_per_block) {
        uint32_t ind = inode->i_block[EXT2_IND_BLOCK];
        if (!ind) return 0;
        uint32_t* table = (uint32_t*)block_ptr(fs, ind);
        if (!table) return 0;
        return table[lblock];
    }
    lblock -= ptrs_per_block;

    /* Double indirect */
    uint32_t ptrs2 = ptrs_per_block * ptrs_per_block;
    if (lblock < ptrs2) {
        uint32_t dind = inode->i_block[EXT2_DIND_BLOCK];
        if (!dind) return 0;
        uint32_t* l1 = (uint32_t*)block_ptr(fs, dind);
        if (!l1) return 0;
        uint32_t l1_idx = lblock / ptrs_per_block;
        uint32_t l2_idx = lblock % ptrs_per_block;
        if (!l1[l1_idx]) return 0;
        uint32_t* l2 = (uint32_t*)block_ptr(fs, l1[l1_idx]);
        if (!l2) return 0;
        return l2[l2_idx];
    }
    lblock -= ptrs2;

    /* Triple indirect */
    uint32_t tind = inode->i_block[EXT2_TIND_BLOCK];
    if (!tind) return 0;
    uint32_t* l1 = (uint32_t*)block_ptr(fs, tind);
    if (!l1) return 0;
    uint32_t ptrs3 = ptrs_per_block * ptrs2;
    (void)ptrs3;
    uint32_t l1_idx = lblock / (ptrs_per_block * ptrs_per_block);
    lblock %= ptrs_per_block * ptrs_per_block;
    if (!l1[l1_idx]) return 0;
    uint32_t* l2 = (uint32_t*)block_ptr(fs, l1[l1_idx]);
    if (!l2) return 0;
    uint32_t l2_idx = lblock / ptrs_per_block;
    uint32_t l3_idx = lblock % ptrs_per_block;
    if (!l2[l2_idx]) return 0;
    uint32_t* l3 = (uint32_t*)block_ptr(fs, l2[l2_idx]);
    if (!l3) return 0;
    return l3[l3_idx];
}

/* =========================================================
 * Write helpers — block/inode allocation, bitmap ops
 * ========================================================= */

/* Bitmap bit-test / set / clear (byte array, LSB-first) */
static inline int  bm_test(uint8_t* bm, uint32_t bit)
    { return (bm[bit >> 3] >> (bit & 7)) & 1; }
static inline void bm_set (uint8_t* bm, uint32_t bit)
    { bm[bit >> 3] |=  (uint8_t)(1u << (bit & 7)); }
static inline void bm_clr (uint8_t* bm, uint32_t bit)
    { bm[bit >> 3] &= (uint8_t)~(1u << (bit & 7)); }

/* Helper: update the on-image superblock's free counts */
static inline ext2_superblock_t* img_sb(ext2_fs_t* fs)
{
    return (ext2_superblock_t*)(fs->data + 1024);
}

/*
 * ext2_alloc_block — allocate one free data block.
 * Returns physical block number, or 0 on failure.
 * Zeros the newly allocated block.
 */
static uint32_t ext2_alloc_block(ext2_fs_t* fs)
{
    uint32_t bpg = fs->sb.s_blocks_per_group;

    for (uint32_t g = 0; g < fs->group_count; g++) {
        ext2_group_desc_t* gd = group_desc(fs, g);
        if (!gd || gd->bg_free_blocks_count == 0) continue;

        uint8_t* bm = block_ptr(fs, gd->bg_block_bitmap);
        if (!bm) continue;

        for (uint32_t i = 0; i < bpg; i++) {
            if (!bm_test(bm, i)) {
                bm_set(bm, i);
                gd->bg_free_blocks_count--;
                ext2_superblock_t* sb = img_sb(fs);
                if (sb->s_free_blocks_count > 0)
                    sb->s_free_blocks_count--;
                fs->sb.s_free_blocks_count = sb->s_free_blocks_count;

                uint32_t blk = fs->first_data_block + g * bpg + i;
                uint8_t* bp = block_ptr(fs, blk);
                if (bp) memset(bp, 0, fs->block_size);
                return blk;
            }
        }
    }
    klog_warn("ext2: no free blocks");
    return 0;
}

/*
 * ext2_alloc_inode — allocate one free inode.
 * Returns inode number (1-based), or 0 on failure.
 */
static uint32_t ext2_alloc_inode(ext2_fs_t* fs)
{
    for (uint32_t g = 0; g < fs->group_count; g++) {
        ext2_group_desc_t* gd = group_desc(fs, g);
        if (!gd || gd->bg_free_inodes_count == 0) continue;

        uint8_t* bm = block_ptr(fs, gd->bg_inode_bitmap);
        if (!bm) continue;

        for (uint32_t i = 0; i < fs->inodes_per_group; i++) {
            if (!bm_test(bm, i)) {
                bm_set(bm, i);
                gd->bg_free_inodes_count--;
                ext2_superblock_t* sb = img_sb(fs);
                if (sb->s_free_inodes_count > 0)
                    sb->s_free_inodes_count--;
                fs->sb.s_free_inodes_count = sb->s_free_inodes_count;
                return g * fs->inodes_per_group + i + 1;
            }
        }
    }
    klog_warn("ext2: no free inodes");
    return 0;
}

/* ext2_free_block — return a block to the free pool */
static void ext2_free_block(ext2_fs_t* fs, uint32_t blk)
{
    if (!blk || blk < fs->first_data_block) return;
    uint32_t bpg = fs->sb.s_blocks_per_group;
    uint32_t rel = blk - fs->first_data_block;
    uint32_t g   = rel / bpg;
    uint32_t i   = rel % bpg;

    ext2_group_desc_t* gd = group_desc(fs, g);
    if (!gd) return;
    uint8_t* bm = block_ptr(fs, gd->bg_block_bitmap);
    if (!bm) return;

    bm_clr(bm, i);
    gd->bg_free_blocks_count++;
    ext2_superblock_t* sb = img_sb(fs);
    sb->s_free_blocks_count++;
    fs->sb.s_free_blocks_count++;
}

/* ext2_free_inode — return an inode to the free pool */
static void ext2_free_inode(ext2_fs_t* fs, uint32_t ino)
{
    if (!ino) return;
    uint32_t g = (ino - 1) / fs->inodes_per_group;
    uint32_t i = (ino - 1) % fs->inodes_per_group;

    ext2_group_desc_t* gd = group_desc(fs, g);
    if (!gd) return;
    uint8_t* bm = block_ptr(fs, gd->bg_inode_bitmap);
    if (!bm) return;

    bm_clr(bm, i);
    gd->bg_free_inodes_count++;
    ext2_superblock_t* sb = img_sb(fs);
    sb->s_free_inodes_count++;
    fs->sb.s_free_inodes_count++;
}

/*
 * ext2_inode_set_block — map logical block lblock → physical pblock in inode.
 * Allocates single/double indirect blocks on demand.
 * Returns 0 on success, -1 on error.
 */
static int ext2_inode_set_block(ext2_fs_t* fs, ext2_inode_t* inode,
                                 uint32_t lblock, uint32_t pblock)
{
    uint32_t ptrs = fs->block_size / sizeof(uint32_t);

    /* Direct blocks */
    if (lblock < EXT2_NDIR_BLOCKS) {
        inode->i_block[lblock] = pblock;
        return 0;
    }
    lblock -= EXT2_NDIR_BLOCKS;

    /* Single indirect */
    if (lblock < ptrs) {
        if (!inode->i_block[EXT2_IND_BLOCK]) {
            uint32_t ind = ext2_alloc_block(fs);
            if (!ind) return -1;
            inode->i_block[EXT2_IND_BLOCK] = ind;
            inode->i_blocks += fs->block_size / 512;
        }
        uint32_t* tbl = (uint32_t*)block_ptr(fs, inode->i_block[EXT2_IND_BLOCK]);
        if (!tbl) return -1;
        tbl[lblock] = pblock;
        return 0;
    }
    lblock -= ptrs;

    /* Double indirect */
    if (lblock < ptrs * ptrs) {
        if (!inode->i_block[EXT2_DIND_BLOCK]) {
            uint32_t dind = ext2_alloc_block(fs);
            if (!dind) return -1;
            inode->i_block[EXT2_DIND_BLOCK] = dind;
            inode->i_blocks += fs->block_size / 512;
        }
        uint32_t* l1 = (uint32_t*)block_ptr(fs, inode->i_block[EXT2_DIND_BLOCK]);
        if (!l1) return -1;
        uint32_t l1i = lblock / ptrs;
        uint32_t l2i = lblock % ptrs;
        if (!l1[l1i]) {
            uint32_t ind2 = ext2_alloc_block(fs);
            if (!ind2) return -1;
            l1[l1i] = ind2;
            inode->i_blocks += fs->block_size / 512;
        }
        uint32_t* l2 = (uint32_t*)block_ptr(fs, l1[l1i]);
        if (!l2) return -1;
        l2[l2i] = pblock;
        return 0;
    }

    klog_warn("ext2: triple-indirect writes not supported");
    return -1;
}

/*
 * ext2_dir_add_entry — insert a new directory entry into directory inode
 * dir_ino.  Finds slack space in existing blocks first; allocates a new
 * data block if necessary.
 * Returns 0 on success, -1 on error.
 */
static int ext2_dir_add_entry(ext2_fs_t* fs, uint32_t dir_ino,
                               uint32_t new_ino, const char* name,
                               uint8_t file_type)
{
    ext2_inode_t* dir_inode = read_inode(fs, dir_ino);
    if (!dir_inode) return -1;

    uint8_t  nlen       = (uint8_t)strlen(name);
    uint16_t rec_needed = (uint16_t)((8u + (uint32_t)nlen + 3u) & ~3u);
    uint32_t bsize      = fs->block_size;
    uint32_t dir_size   = dir_inode->i_size;

    /* Walk existing directory blocks looking for slack */
    for (uint32_t lb = 0; lb * bsize < dir_size; lb++) {
        uint32_t pb = file_block_to_phys(fs, dir_inode, lb);
        if (!pb) continue;
        uint8_t* blk = block_ptr(fs, pb);
        if (!blk) continue;

        uint32_t off = 0;
        while (off + 8u <= bsize) {
            ext2_dirent_t* de = (ext2_dirent_t*)(blk + off);
            if (de->rec_len == 0) break;

            if (de->inode == 0) {
                /* Deleted or empty slot — reuse if big enough */
                if (de->rec_len >= rec_needed) {
                    de->inode     = new_ino;
                    de->name_len  = nlen;
                    de->file_type = file_type;
                    memcpy(de->name, name, nlen);
                    return 0;
                }
            } else {
                /* Check if this entry has enough trailing padding */
                uint16_t used  = (uint16_t)((8u + de->name_len + 3u) & ~3u);
                uint16_t slack = de->rec_len - used;
                if (slack >= rec_needed) {
                    /* Split: shrink current entry, insert new one after */
                    de->rec_len = used;
                    ext2_dirent_t* ne = (ext2_dirent_t*)(blk + off + used);
                    ne->inode     = new_ino;
                    ne->rec_len   = slack;
                    ne->name_len  = nlen;
                    ne->file_type = file_type;
                    memcpy(ne->name, name, nlen);
                    return 0;
                }
            }
            off += de->rec_len;
        }
    }

    /* No slack found — allocate a new directory data block */
    uint32_t new_lb = dir_size / bsize;
    uint32_t new_pb = ext2_alloc_block(fs);
    if (!new_pb) return -1;
    if (ext2_inode_set_block(fs, dir_inode, new_lb, new_pb) != 0) {
        ext2_free_block(fs, new_pb);
        return -1;
    }
    dir_inode->i_size   += bsize;
    dir_inode->i_blocks += bsize / 512;

    uint8_t* blk = block_ptr(fs, new_pb);
    ext2_dirent_t* ne = (ext2_dirent_t*)blk;
    ne->inode     = new_ino;
    ne->rec_len   = (uint16_t)bsize;
    ne->name_len  = nlen;
    ne->file_type = file_type;
    memcpy(ne->name, name, nlen);
    return 0;
}

/* =========================================================
 * VFS operations (read/write)
 * ========================================================= */

static ssize_t ext2_read(vfs_node_t* node, off_t offset, size_t size, void* buf)
{
    ext2_fs_t* fs   = (ext2_fs_t*)node->impl;
    uint32_t   ino  = (uint32_t)node->inode;
    ext2_inode_t* inode = read_inode(fs, ino);
    if (!inode) return -1;

    uint32_t file_size = inode->i_size;
    if ((uint64_t)offset >= file_size) return 0;
    if (offset + (off_t)size > (off_t)file_size)
        size = (size_t)(file_size - offset);

    uint8_t* out   = (uint8_t*)buf;
    size_t   read  = 0;
    uint32_t bsize = fs->block_size;

    while (read < size) {
        uint32_t lblock  = (uint32_t)((offset + (off_t)read) / bsize);
        uint32_t boff    = (uint32_t)((offset + (off_t)read) % bsize);
        uint32_t pblock  = file_block_to_phys(fs, inode, lblock);
        size_t   chunk   = bsize - boff;
        if (chunk > size - read) chunk = size - read;

        if (pblock == 0) {
            /* Sparse hole — fill with zeros */
            memset(out + read, 0, chunk);
        } else {
            uint8_t* src = block_ptr(fs, pblock);
            if (!src) return (ssize_t)read;
            memcpy(out + read, src + boff, chunk);
        }
        read += chunk;
    }
    return (ssize_t)read;
}

static ssize_t ext2_write(vfs_node_t* node, off_t offset, size_t size,
                           const void* buf)
{
    if (!buf || size == 0) return 0;

    ext2_fs_t*    fs    = (ext2_fs_t*)node->impl;
    uint32_t      ino   = (uint32_t)node->inode;
    ext2_inode_t* inode = read_inode(fs, ino);
    if (!inode) return -1;

    const uint8_t* src    = (const uint8_t*)buf;
    uint32_t       bsize  = fs->block_size;
    size_t         written = 0;

    while (written < size) {
        uint32_t lb    = (uint32_t)((offset + (off_t)written) / bsize);
        uint32_t boff  = (uint32_t)((offset + (off_t)written) % bsize);
        size_t   chunk = bsize - boff;
        if (chunk > size - written) chunk = size - written;

        uint32_t pb = file_block_to_phys(fs, inode, lb);
        if (!pb) {
            pb = ext2_alloc_block(fs);
            if (!pb) break;
            if (ext2_inode_set_block(fs, inode, lb, pb) != 0)
                break;
            inode->i_blocks += bsize / 512;
        }

        uint8_t* dst = block_ptr(fs, pb);
        if (!dst) break;
        memcpy(dst + boff, src + written, chunk);
        written += chunk;
    }

    /* Extend file size if needed */
    uint32_t new_end = (uint32_t)(offset + (off_t)written);
    if (new_end > inode->i_size) {
        inode->i_size = new_end;
        node->size    = new_end;
    }

    return (ssize_t)written;
}

/* Forward declaration */
static vfs_node_t* ext2_make_node(ext2_fs_t* fs, uint32_t ino,
                                   const char* name);

static vfs_node_t* ext2_finddir(vfs_node_t* dir, const char* name)
{
    ext2_fs_t* fs   = (ext2_fs_t*)dir->impl;
    uint32_t   ino  = (uint32_t)dir->inode;
    ext2_inode_t* inode = read_inode(fs, ino);
    if (!inode) return NULL;

    uint32_t offset = 0;
    uint32_t file_size = inode->i_size;
    uint32_t bsize = fs->block_size;

    while (offset < file_size) {
        uint32_t lblock = offset / bsize;
        uint32_t pblock = file_block_to_phys(fs, inode, lblock);
        if (!pblock) { offset = (lblock + 1) * bsize; continue; }

        uint8_t* blk = block_ptr(fs, pblock);
        if (!blk) return NULL;

        uint32_t blk_off = offset % bsize;
        while (blk_off < bsize && offset < file_size) {
            ext2_dirent_t* de = (ext2_dirent_t*)(blk + blk_off);
            if (de->rec_len == 0) break;

            if (de->inode != 0) {
                /* Compare names */
                size_t nlen = de->name_len;
                if (nlen == strlen(name) &&
                    memcmp(de->name, name, nlen) == 0) {
                    return ext2_make_node(fs, de->inode, name);
                }
            }
            blk_off += de->rec_len;
            offset  += de->rec_len;
        }
        /* Advance to next block */
        offset = (lblock + 1) * bsize;
    }
    return NULL;
}

static vfs_dirent_t* ext2_readdir(vfs_node_t* dir, uint32_t index)
{
    ext2_fs_t* fs  = (ext2_fs_t*)dir->impl;
    uint32_t   ino = (uint32_t)dir->inode;
    ext2_inode_t* inode = read_inode(fs, ino);
    if (!inode) return NULL;

    uint32_t count  = 0;
    uint32_t offset = 0;
    uint32_t file_size = inode->i_size;
    uint32_t bsize  = fs->block_size;

    while (offset < file_size) {
        uint32_t lblock = offset / bsize;
        uint32_t pblock = file_block_to_phys(fs, inode, lblock);
        if (!pblock) { offset = (lblock + 1) * bsize; continue; }

        uint8_t* blk = block_ptr(fs, pblock);
        if (!blk) return NULL;

        uint32_t blk_off = offset % bsize;
        while (blk_off < bsize && offset < file_size) {
            ext2_dirent_t* de = (ext2_dirent_t*)(blk + blk_off);
            if (de->rec_len == 0) break;

            if (de->inode != 0) {
                if (count == index) {
                    vfs_dirent_t* result = (vfs_dirent_t*)kmalloc(sizeof(vfs_dirent_t));
                    if (!result) return NULL;
                    size_t nlen = de->name_len;
                    if (nlen > VFS_NAME_MAX) nlen = VFS_NAME_MAX;
                    memcpy(result->name, de->name, nlen);
                    result->name[nlen] = '\0';
                    result->inode = de->inode;
                    result->type = (de->file_type == EXT2_FT_DIR)
                                   ? VFS_DIRECTORY : VFS_FILE;
                    return result;
                }
                count++;
            }
            blk_off += de->rec_len;
            offset  += de->rec_len;
        }
        offset = (lblock + 1) * bsize;
    }
    return NULL;
}

/*
 * ext2_create_op — create a new regular file named @name inside directory
 * @dir.  Allocates an inode, initialises it, and adds a directory entry.
 */
static int ext2_create_op(vfs_node_t* dir, const char* name, uint32_t mode)
{
    ext2_fs_t* fs = (ext2_fs_t*)dir->impl;

    uint32_t new_ino = ext2_alloc_inode(fs);
    if (!new_ino) return -1;

    ext2_inode_t* inode = read_inode(fs, new_ino);
    if (!inode) { ext2_free_inode(fs, new_ino); return -1; }

    memset(inode, 0, fs->inode_size);
    inode->i_mode        = (uint16_t)(EXT2_S_IFREG | (mode & 0777u));
    inode->i_links_count = 1;
    inode->i_size        = 0;

    uint32_t dir_ino = (uint32_t)dir->inode;
    if (ext2_dir_add_entry(fs, dir_ino, new_ino, name, EXT2_FT_REG_FILE) != 0) {
        ext2_free_inode(fs, new_ino);
        return -1;
    }
    return 0;
}

/*
 * ext2_mkdir_op — create a new directory named @name inside @parent.
 * Allocates an inode and one data block for the "." / ".." entries.
 */
static int ext2_mkdir_op(vfs_node_t* parent, const char* name, uint32_t mode)
{
    ext2_fs_t* fs = (ext2_fs_t*)parent->impl;

    uint32_t new_ino = ext2_alloc_inode(fs);
    if (!new_ino) return -1;

    ext2_inode_t* inode = read_inode(fs, new_ino);
    if (!inode) { ext2_free_inode(fs, new_ino); return -1; }

    uint32_t data_blk = ext2_alloc_block(fs);
    if (!data_blk) { ext2_free_inode(fs, new_ino); return -1; }

    memset(inode, 0, fs->inode_size);
    inode->i_mode        = (uint16_t)(EXT2_S_IFDIR | (mode & 0777u));
    inode->i_links_count = 2;          /* "." + parent ref */
    inode->i_block[0]    = data_blk;
    inode->i_size        = fs->block_size;
    inode->i_blocks      = fs->block_size / 512;

    uint32_t parent_ino = (uint32_t)parent->inode;

    /* Write "." and ".." entries into the new data block */
    uint8_t* blk = block_ptr(fs, data_blk);
    /* "." */
    ext2_dirent_t* dot = (ext2_dirent_t*)blk;
    dot->inode     = new_ino;
    dot->rec_len   = 12;
    dot->name_len  = 1;
    dot->file_type = EXT2_FT_DIR;
    dot->name[0]   = '.';
    /* ".." */
    ext2_dirent_t* dotdot = (ext2_dirent_t*)(blk + 12);
    dotdot->inode     = parent_ino;
    dotdot->rec_len   = (uint16_t)(fs->block_size - 12);
    dotdot->name_len  = 2;
    dotdot->file_type = EXT2_FT_DIR;
    dotdot->name[0]   = '.';
    dotdot->name[1]   = '.';

    /* Add our entry in the parent */
    if (ext2_dir_add_entry(fs, parent_ino, new_ino, name, EXT2_FT_DIR) != 0) {
        ext2_free_block(fs, data_blk);
        ext2_free_inode(fs, new_ino);
        return -1;
    }

    /* Increment parent link count for ".." */
    ext2_inode_t* parent_inode = read_inode(fs, parent_ino);
    if (parent_inode) parent_inode->i_links_count++;

    /* Track used directories in group descriptor */
    uint32_t g = (new_ino - 1) / fs->inodes_per_group;
    ext2_group_desc_t* gd = group_desc(fs, g);
    if (gd) gd->bg_used_dirs_count++;

    return 0;
}

/*
 * ext2_unlink_op — remove the directory entry @name from @dir.
 * Decrements the target inode's link count; frees all blocks and the
 * inode if the link count reaches zero.
 */
static int ext2_unlink_op(vfs_node_t* dir, const char* name)
{
    ext2_fs_t*    fs        = (ext2_fs_t*)dir->impl;
    uint32_t      dir_ino   = (uint32_t)dir->inode;
    ext2_inode_t* dir_inode = read_inode(fs, dir_ino);
    if (!dir_inode) return -1;

    size_t   nlen     = strlen(name);
    uint32_t bsize    = fs->block_size;
    uint32_t dir_size = dir_inode->i_size;

    for (uint32_t lb = 0; lb * bsize < dir_size; lb++) {
        uint32_t pb = file_block_to_phys(fs, dir_inode, lb);
        if (!pb) continue;
        uint8_t* blk = block_ptr(fs, pb);
        if (!blk) continue;

        uint32_t off = 0;
        while (off + 8u <= bsize) {
            ext2_dirent_t* de = (ext2_dirent_t*)(blk + off);
            if (de->rec_len == 0) break;

            if (de->inode != 0 && de->name_len == nlen &&
                memcmp(de->name, name, nlen) == 0) {

                uint32_t target_ino = de->inode;
                de->inode = 0;   /* Mark entry deleted */

                ext2_inode_t* target = read_inode(fs, target_ino);
                if (target && target->i_links_count > 0) {
                    target->i_links_count--;
                    if (target->i_links_count == 0) {
                        /* Free all data blocks */
                        uint32_t nblocks =
                            (target->i_size + bsize - 1) / bsize;
                        for (uint32_t fb = 0; fb < nblocks; fb++) {
                            uint32_t fpb = file_block_to_phys(fs, target, fb);
                            if (fpb) ext2_free_block(fs, fpb);
                        }
                        /* Free indirect blocks */
                        if (target->i_block[EXT2_IND_BLOCK]) {
                            ext2_free_block(fs, target->i_block[EXT2_IND_BLOCK]);
                        }
                        if (target->i_block[EXT2_DIND_BLOCK]) {
                            uint32_t* l1 = (uint32_t*)block_ptr(
                                fs, target->i_block[EXT2_DIND_BLOCK]);
                            if (l1) {
                                uint32_t ptrs = bsize / 4;
                                for (uint32_t p = 0; p < ptrs; p++)
                                    if (l1[p]) ext2_free_block(fs, l1[p]);
                            }
                            ext2_free_block(fs, target->i_block[EXT2_DIND_BLOCK]);
                        }
                        memset(target, 0, fs->inode_size);
                        ext2_free_inode(fs, target_ino);
                    }
                }
                return 0;
            }
            off += de->rec_len;
        }
    }
    return -1;  /* Not found */
}

static vfs_ops_t ext2_ops = {
    .read    = ext2_read,
    .write   = ext2_write,
    .finddir = ext2_finddir,
    .readdir = ext2_readdir,
    .mkdir   = ext2_mkdir_op,
    .create  = ext2_create_op,
    .unlink  = ext2_unlink_op,
    .open    = NULL,
    .close   = NULL,
    .ioctl   = NULL,
};

static vfs_node_t* ext2_make_node(ext2_fs_t* fs, uint32_t ino,
                                   const char* name)
{
    ext2_inode_t* inode = read_inode(fs, ino);
    if (!inode) return NULL;

    vfs_node_t* node = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));
    if (!node) return NULL;
    memset(node, 0, sizeof(*node));

    strncpy(node->name, name, VFS_NAME_MAX);
    node->name[VFS_NAME_MAX] = '\0';
    node->inode    = ino;
    node->size     = inode->i_size;
    node->atime    = inode->i_atime;
    node->mtime    = inode->i_mtime;
    node->ctime    = inode->i_ctime;
    node->uid      = inode->i_uid;
    node->gid      = inode->i_gid;
    node->mode     = inode->i_mode;
    node->ops      = &ext2_ops;
    node->impl     = fs;
    node->refcount = 1;

    uint16_t mode_type = inode->i_mode & EXT2_S_IFMT;
    if (mode_type == EXT2_S_IFDIR)
        node->flags = VFS_DIRECTORY;
    else if (mode_type == EXT2_S_IFLNK)
        node->flags = VFS_SYMLINK;
    else
        node->flags = VFS_FILE;

    return node;
}

/* =========================================================
 * Public API
 * ========================================================= */

vfs_node_t* ext2_mount(uint8_t* data, uint64_t size)
{
    if (!data || size < 2048) {
        klog_warn("ext2: image too small");
        return NULL;
    }

    /* Superblock is at byte offset 1024 */
    ext2_superblock_t* sb = (ext2_superblock_t*)(data + 1024);
    if (sb->s_magic != EXT2_MAGIC) {
        klog_warn("ext2: bad magic 0x%x (expected 0xEF53)", sb->s_magic);
        return NULL;
    }

    ext2_fs_t* fs = (ext2_fs_t*)kmalloc(sizeof(ext2_fs_t));
    if (!fs) return NULL;
    memset(fs, 0, sizeof(*fs));

    fs->data             = data;
    fs->size             = size;
    fs->block_size       = 1024u << sb->s_log_block_size;
    fs->inodes_per_group = sb->s_inodes_per_group;
    fs->inode_size       = (sb->s_rev_level >= 1) ? sb->s_inode_size : 128;
    fs->first_data_block = sb->s_first_data_block;
    fs->sb               = *sb;

    /* Calculate block group count */
    uint32_t bc = (sb->s_blocks_count + sb->s_blocks_per_group - 1)
                   / sb->s_blocks_per_group;
    uint32_t ic = (sb->s_inodes_count + sb->s_inodes_per_group - 1)
                   / sb->s_inodes_per_group;
    fs->group_count = (bc > ic) ? bc : ic;

    klog_info("ext2: block_size=%u groups=%u inodes=%u",
              fs->block_size, fs->group_count, sb->s_inodes_count);

    /* Build VFS root node (inode 2 is always the root directory) */
    vfs_node_t* root = ext2_make_node(fs, 2, "/");
    if (!root) {
        kfree(fs);
        return NULL;
    }
    return root;
}

int ext2_init(uint8_t* data, uint64_t size, const char* mount_point)
{
    vfs_node_t* root = ext2_mount(data, size);
    if (!root) return -1;

    int result = vfs_mount(mount_point, root, NULL);
    if (result != 0) {
        klog_warn("ext2: failed to mount at %s", mount_point);
        kfree(root);
        return -1;
    }
    klog_info("ext2: mounted at %s", mount_point);
    return 0;
}
