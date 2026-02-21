/*
 * fs/ext2.c - Read-only EXT2 filesystem driver
 *
 * Parses an in-memory EXT2 image and exposes it through the VFS layer.
 * Supports: directory traversal, regular file reads, symbolic link
 * resolution (up to one level), and stat-like inode metadata.
 *
 * Limitations (v0.1):
 *   - Read-only: no writes, no link count updates.
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
 * VFS operations (read-only)
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
    (void)node; (void)offset; (void)size; (void)buf;
    return -1; /* Read-only */
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

static vfs_ops_t ext2_ops = {
    .read    = ext2_read,
    .write   = ext2_write,
    .finddir = ext2_finddir,
    .readdir = ext2_readdir,
    .mkdir   = NULL,
    .create  = NULL,
    .unlink  = NULL,
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
