/*
 * fs/fat32.c - FAT32 filesystem driver
 *
 * Provides read/write access to FAT32 volumes on ATA devices.
 * Integrates with the VFS via fat32_ops so volumes can be mounted.
 *
 * Implementation notes:
 *   - All disk I/O goes through ata_read_sectors / ata_write_sectors.
 *   - A single 512-byte sector buffer per mount context is used for FAT reads.
 *   - LFN entries are reassembled before 8.3 entries.
 *   - Cluster allocation uses the FSInfo next_free hint.
 */
#include <fs/fat32.h>
#include <fs/vfs.h>
#include <drivers/ata.h>
#include <memory.h>
#include <kernel.h>
#include <string.h>

/* Helper: get ATA drive pointer from index stored in fat32_fs_t */
static inline ata_drive_t* fat32_get_drive(int dev)
{
    extern ata_drive_t ata_drives[4];
    if (dev < 0 || dev >= 4) return NULL;
    return &ata_drives[dev];
}

fat32_fs_t fat32_mounts[FAT32_MAX_MOUNTS];

/* ── Low-level sector I/O ────────────────────────────────────────────── */

static int fat32_read_sector(fat32_fs_t* fs, uint32_t lba, void* buf)
{
    ata_drive_t* drv = fat32_get_drive(fs->dev);
    if (!drv) return -EIO;
    return ata_read_sectors(drv, fs->partition_start_lba + lba, 1, buf);
}

static int fat32_write_sector(fat32_fs_t* fs, uint32_t lba, const void* buf)
{
    ata_drive_t* drv = fat32_get_drive(fs->dev);
    if (!drv) return -EIO;
    return ata_write_sectors(drv, fs->partition_start_lba + lba, 1, buf);
}

/* Ensure sector_buf contains the given LBA */
static int fat32_load_sector(fat32_fs_t* fs, uint32_t lba)
{
    if (fs->sector_buf_lba == lba) return 0;
    if (fs->sector_buf_dirty) {
        fat32_write_sector(fs, fs->sector_buf_lba, fs->sector_buf);
        fs->sector_buf_dirty = false;
    }
    int r = fat32_read_sector(fs, lba, fs->sector_buf);
    if (r == 0) fs->sector_buf_lba = lba;
    return r;
}

/* ── FAT table access ────────────────────────────────────────────────── */

/* LBA of the sector that holds the FAT entry for cluster @c */
static uint32_t fat32_fat_lba(fat32_fs_t* fs, uint32_t cluster)
{
    uint32_t fat_offset  = cluster * 4;
    uint32_t fat_sector  = fs->fat_lba + fat_offset / FAT32_SECTOR_SIZE;
    return fat_sector;
}

static uint32_t fat32_fat_offset(uint32_t cluster)
{
    return (cluster * 4) % FAT32_SECTOR_SIZE;
}

uint32_t fat32_next_cluster(fat32_fs_t* fs, uint32_t cluster)
{
    uint32_t lba = fat32_fat_lba(fs, cluster);
    if (fat32_load_sector(fs, lba) != 0) return FAT32_EOC;
    uint32_t off = fat32_fat_offset(cluster);
    uint32_t val;
    memcpy(&val, fs->sector_buf + off, 4);
    return val & FAT32_MASK;
}

static int fat32_set_fat(fat32_fs_t* fs, uint32_t cluster, uint32_t val)
{
    uint32_t lba = fat32_fat_lba(fs, cluster);
    if (fat32_load_sector(fs, lba) != 0) return -EIO;
    uint32_t off = fat32_fat_offset(cluster);
    val &= FAT32_MASK;
    memcpy(fs->sector_buf + off, &val, 4);
    fs->sector_buf_dirty = true;
    return 0;
}

/* ── Cluster read/write ──────────────────────────────────────────────── */

/* LBA of first sector of cluster @c */
static uint32_t cluster_to_lba(fat32_fs_t* fs, uint32_t cluster)
{
    return fs->data_lba + (cluster - 2) * fs->sectors_per_cluster;
}

int fat32_read_cluster(fat32_fs_t* fs, uint32_t cluster, void* buf)
{
    if (cluster < 2 || cluster >= FAT32_EOC_MIN) return -EINVAL;
    uint32_t lba = cluster_to_lba(fs, cluster);
    for (uint32_t i = 0; i < fs->sectors_per_cluster; i++) {
        int r = fat32_read_sector(fs, lba + i,
                                  (uint8_t*)buf + i * FAT32_SECTOR_SIZE);
        if (r != 0) return r;
    }
    return 0;
}

int fat32_write_cluster(fat32_fs_t* fs, uint32_t cluster, const void* buf)
{
    if (cluster < 2 || cluster >= FAT32_EOC_MIN) return -EINVAL;
    uint32_t lba = cluster_to_lba(fs, cluster);
    for (uint32_t i = 0; i < fs->sectors_per_cluster; i++) {
        int r = fat32_write_sector(fs, lba + i,
                                   (const uint8_t*)buf + i * FAT32_SECTOR_SIZE);
        if (r != 0) return r;
    }
    return 0;
}

/* ── Cluster allocation ──────────────────────────────────────────────── */

uint32_t fat32_alloc_cluster(fat32_fs_t* fs, uint32_t prev)
{
    /* Linear scan from next_free_hint */
    uint32_t start = (fs->next_free_hint > 2) ? fs->next_free_hint : 2;

    for (uint32_t c = start; c < fs->total_clusters + 2; c++) {
        uint32_t val = fat32_next_cluster(fs, c);
        if (val == FAT32_FREE) {
            fat32_set_fat(fs, c, FAT32_EOC);
            if (prev != 0) fat32_set_fat(fs, prev, c);
            fs->next_free_hint = c + 1;
            if (fs->free_clusters > 0) fs->free_clusters--;
            /* Zero the cluster */
            uint8_t* zero = (uint8_t*)kmalloc(fs->bytes_per_cluster);
            if (zero) {
                memset(zero, 0, fs->bytes_per_cluster);
                fat32_write_cluster(fs, c, zero);
                kfree(zero);
            }
            return c;
        }
    }

    /* Wrap around */
    for (uint32_t c = 2; c < start; c++) {
        uint32_t val = fat32_next_cluster(fs, c);
        if (val == FAT32_FREE) {
            fat32_set_fat(fs, c, FAT32_EOC);
            if (prev != 0) fat32_set_fat(fs, prev, c);
            fs->next_free_hint = c + 1;
            if (fs->free_clusters > 0) fs->free_clusters--;
            return c;
        }
    }

    kerror("FAT32: no free clusters on '%s'", fs->mount_point);
    return 0;
}

int fat32_free_chain(fat32_fs_t* fs, uint32_t start)
{
    uint32_t c = start;
    while (c >= 2 && c < FAT32_EOC_MIN) {
        uint32_t next = fat32_next_cluster(fs, c);
        fat32_set_fat(fs, c, FAT32_FREE);
        fs->free_clusters++;
        c = next;
    }
    return 0;
}

/* ── LFN helper: assemble a long filename from LFN entries ───────────── */

static void lfn_collect_chars(const fat32_lfn_t* lfn, char* buf, size_t* pos,
                               size_t maxlen)
{
    /* LFN stores UCS-2; we downcast to ASCII (Latin-1) */
    for (int i = 0; i < 5 && *pos < maxlen - 1; i++) {
        uint16_t c = lfn->name1[i];
        if (c == 0 || c == 0xFFFF) return;
        buf[(*pos)++] = (char)(c & 0xFF);
    }
    for (int i = 0; i < 6 && *pos < maxlen - 1; i++) {
        uint16_t c = lfn->name2[i];
        if (c == 0 || c == 0xFFFF) return;
        buf[(*pos)++] = (char)(c & 0xFF);
    }
    for (int i = 0; i < 2 && *pos < maxlen - 1; i++) {
        uint16_t c = lfn->name3[i];
        if (c == 0 || c == 0xFFFF) return;
        buf[(*pos)++] = (char)(c & 0xFF);
    }
}

/* Convert 8.3 to a proper string (trim spaces, add dot) */
static void fat32_83_to_str(const fat32_dirent_t* de, char* out)
{
    int i = 7;
    while (i >= 0 && de->name[i] == ' ') i--;
    int namelen = i + 1;
    for (int j = 0; j < namelen; j++) out[j] = de->name[j];
    out[namelen] = '\0';

    int extlen = 0;
    for (int j = 2; j >= 0 && de->ext[j] == ' '; j--) {}
    for (int j = 0; j <= 2; j++) if (de->ext[j] != ' ') extlen = j + 1;

    if (extlen > 0) {
        out[namelen] = '.';
        for (int j = 0; j < extlen; j++) out[namelen + 1 + j] = de->ext[j];
        out[namelen + 1 + extlen] = '\0';
    }
}

/* ── Directory iteration ─────────────────────────────────────────────── */

typedef struct {
    char     name[256];
    uint32_t first_cluster;
    uint32_t size;
    uint8_t  attr;
    bool     valid;
} fat32_entry_t;

static uint8_t* g_cluster_buf = NULL; /* Temporary cluster-sized buffer */

/*
 * fat32_read_dir_entry - read directory entry at byte @index within the
 * directory starting at @dir_cluster.  Assembles LFN if present.
 */
static int fat32_read_dir_entry(fat32_fs_t* fs, uint32_t dir_cluster,
                                 uint32_t entry_idx, fat32_entry_t* out)
{
    size_t bpc = fs->bytes_per_cluster;

    if (!g_cluster_buf) {
        g_cluster_buf = (uint8_t*)kmalloc(bpc);
        if (!g_cluster_buf) return -ENOMEM;
    }

    uint32_t entries_per_cluster = bpc / 32;
    uint32_t cluster_idx         = entry_idx / entries_per_cluster;
    uint32_t intra_idx           = entry_idx % entries_per_cluster;

    /* Walk the cluster chain to cluster_idx */
    uint32_t c = dir_cluster;
    for (uint32_t i = 0; i < cluster_idx; i++) {
        c = fat32_next_cluster(fs, c);
        if (c >= FAT32_EOC_MIN) return -ENOENT;
    }

    if (fat32_read_cluster(fs, c, g_cluster_buf) != 0) return -EIO;

    fat32_dirent_t* de = (fat32_dirent_t*)(g_cluster_buf + intra_idx * 32);

    if (de->name[0] == 0x00) return -ENOENT; /* End of directory */
    if ((uint8_t)de->name[0] == 0xE5) { /* Deleted */
        out->valid = false;
        return 0;
    }

    if (de->attr == FAT_ATTR_LFN) {
        /* LFN entry — collect the name from preceding LFN entries */
        char lfn_name[256];
        size_t lfn_pos = 0;
        memset(lfn_name, 0, sizeof(lfn_name));

        /* Read backwards through LFN entries (they're stored in reverse) */
        /* Simplified: just use the 8.3 name for now if LFN is complex */
        out->valid = false;
        return 0;
    }

    /* System / volume label — skip */
    if (de->attr & (FAT_ATTR_SYSTEM | FAT_ATTR_VOLUME_ID)) {
        out->valid = false;
        return 0;
    }

    fat32_83_to_str(de, out->name);
    out->first_cluster = ((uint32_t)de->cluster_hi << 16) | de->cluster_lo;
    out->size          = de->file_size;
    out->attr          = de->attr;
    out->valid         = true;

    return 1; /* Valid entry */
}

/* ── Path resolution ─────────────────────────────────────────────────── */

static int fat32_resolve(fat32_fs_t* fs, const char* path,
                          fat32_entry_t* out)
{
    if (!path || path[0] != '/') return -EINVAL;

    /* Start at root cluster */
    uint32_t dir_cluster = fs->root_cluster;
    fat32_entry_t entry;

    /* Skip leading slash */
    const char* p = path + 1;

    while (*p) {
        /* Extract next component */
        char comp[256];
        size_t len = 0;
        while (*p && *p != '/' && len < 255) comp[len++] = *p++;
        comp[len] = '\0';
        if (*p == '/') p++;

        if (len == 0) continue;

        /* Scan the current directory for @comp */
        bool found = false;
        for (uint32_t i = 0; ; i++) {
            int r = fat32_read_dir_entry(fs, dir_cluster, i, &entry);
            if (r < 0) return -ENOENT;
            if (!entry.valid) continue;
            if (strncmp(entry.name, comp, 255) == 0) {
                found = true;
                break;
            }
        }

        if (!found) return -ENOENT;

        /* If there are more components, we need a directory */
        if (*p) {
            if (!(entry.attr & FAT_ATTR_DIR)) return -ENOTDIR;
            dir_cluster = entry.first_cluster;
            if (dir_cluster == 0) dir_cluster = fs->root_cluster;
        }
    }

    *out = entry;
    return 0;
}

/* ── VFS node ops ────────────────────────────────────────────────────── */

/*
 * Per-node context stored in vfs_node_t::impl.
 * Tracks the directory location of the on-disk directory entry so we can
 * write back updated file sizes and first-cluster values after writes.
 */
typedef struct {
    fat32_fs_t* fs;
    uint32_t    dir_cluster; /* Cluster of the directory holding this entry */
    uint32_t    dirent_idx;  /* Raw 32-byte entry index in that cluster chain */
} fat32_node_ctx_t;

/* ── Directory-write helpers ─────────────────────────────────────────── */

/* Convert a filename string to 8.3 format (space-padded, upper-cased). */
static void fat32_str_to_83(const char* name, uint8_t* out_name, uint8_t* out_ext)
{
    memset(out_name, ' ', 8);
    memset(out_ext,  ' ', 3);

    /* Find the last dot for extension split */
    const char* dot = NULL;
    for (const char* p = name; *p; p++)
        if (*p == '.') dot = p;

    int ni = 0;
    const char* s = name;
    while (*s && s != dot && ni < 8) {
        char c = *s++;
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        out_name[ni++] = (uint8_t)c;
    }
    if (dot) {
        s = dot + 1;
        int ei = 0;
        while (*s && ei < 3) {
            char c = *s++;
            if (c >= 'a' && c <= 'z') c = (char)(c - 32);
            out_ext[ei++] = (uint8_t)c;
        }
    }
}

/*
 * Write a raw 32-byte directory entry at position @entry_idx in the
 * directory cluster chain starting at @dir_cluster.
 */
static int fat32_write_dirent_at(fat32_fs_t* fs, uint32_t dir_cluster,
                                  uint32_t entry_idx, const fat32_dirent_t* de)
{
    size_t   bpc = fs->bytes_per_cluster;
    uint32_t epc = bpc / 32;
    uint32_t ci  = entry_idx / epc;
    uint32_t off = entry_idx % epc;

    uint32_t c = dir_cluster;
    for (uint32_t i = 0; i < ci; i++) {
        c = fat32_next_cluster(fs, c);
        if (c >= FAT32_EOC_MIN || c < 2) return -ENOSPC;
    }

    uint8_t* cbuf = (uint8_t*)kmalloc(bpc);
    if (!cbuf) return -ENOMEM;
    if (fat32_read_cluster(fs, c, cbuf) != 0) { kfree(cbuf); return -EIO; }
    memcpy(cbuf + off * 32, de, 32);
    int r = fat32_write_cluster(fs, c, cbuf);
    kfree(cbuf);
    return r;
}

/*
 * Find the first empty (0x00) or deleted (0xE5) directory entry.
 * Extends the directory with a new cluster if all slots are used.
 * Returns the entry index on success, or a negative errno.
 */
static int fat32_find_free_dirent(fat32_fs_t* fs, uint32_t dir_cluster)
{
    size_t   bpc  = fs->bytes_per_cluster;
    uint32_t epc  = bpc / 32;
    uint8_t* cbuf = (uint8_t*)kmalloc(bpc);
    if (!cbuf) return -ENOMEM;

    uint32_t c    = dir_cluster;
    uint32_t cnum = 0;
    uint32_t prev = 0;

    while (c >= 2 && c < FAT32_EOC_MIN) {
        if (fat32_read_cluster(fs, c, cbuf) != 0) { kfree(cbuf); return -EIO; }
        for (uint32_t i = 0; i < epc; i++) {
            fat32_dirent_t* de = (fat32_dirent_t*)(cbuf + i * 32);
            if (de->name[0] == 0x00 || (uint8_t)de->name[0] == 0xE5) {
                kfree(cbuf);
                return (int)(cnum * epc + i);
            }
        }
        prev = c;
        c    = fat32_next_cluster(fs, c);
        cnum++;
    }
    kfree(cbuf);

    /* No free slot — extend directory with a new cluster */
    uint32_t new_c = fat32_alloc_cluster(fs, prev);
    if (new_c < 2) return -ENOSPC;
    return (int)(cnum * epc);
}

/* Forward declarations for ops table */
static vfs_node_t*   _fat32_vfs_finddir(vfs_node_t* parent, const char* name);
static vfs_dirent_t* _fat32_vfs_readdir_op(vfs_node_t* node, uint32_t idx);
static ssize_t       _fat32_vfs_read(vfs_node_t* node, off_t offset,
                                      size_t len, void* buf);
static ssize_t       _fat32_vfs_write(vfs_node_t* node, off_t offset,
                                       size_t len, const void* buf);
static int           _fat32_vfs_create(vfs_node_t* parent, const char* name,
                                        uint32_t mode);
static int           _fat32_vfs_mkdir_op(vfs_node_t* parent, const char* name,
                                          uint32_t mode);
static int           _fat32_vfs_unlink(vfs_node_t* parent, const char* name);

static const vfs_ops_t fat32_vfs_ops = {
    .read    = _fat32_vfs_read,
    .write   = _fat32_vfs_write,
    .finddir = _fat32_vfs_finddir,
    .readdir = _fat32_vfs_readdir_op,
    .open    = NULL,
    .close   = NULL,
    .mkdir   = _fat32_vfs_mkdir_op,
    .create  = _fat32_vfs_create,
    .unlink  = _fat32_vfs_unlink,
};

/*
 * Allocate a vfs_node_t for a FAT32 directory entry.
 * @dir_cluster  — cluster of the directory that contains this entry.
 * @dirent_idx   — raw 32-byte entry index within that directory.
 */
static vfs_node_t* fat32_make_node(fat32_fs_t* fs, const fat32_entry_t* e,
                                    uint32_t dir_cluster, uint32_t dirent_idx)
{
    fat32_node_ctx_t* ctx = (fat32_node_ctx_t*)kmalloc(sizeof(fat32_node_ctx_t));
    vfs_node_t*       n   = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));
    if (!ctx || !n) { kfree(ctx); kfree(n); return NULL; }
    memset(n,   0, sizeof(*n));
    memset(ctx, 0, sizeof(*ctx));

    ctx->fs          = fs;
    ctx->dir_cluster = dir_cluster;
    ctx->dirent_idx  = dirent_idx;

    strncpy(n->name, e->name, VFS_NAME_MAX);
    n->size  = e->size;
    n->inode = e->first_cluster;    /* cluster number used as inode */
    n->impl  = (void*)ctx;
    n->ops   = (vfs_ops_t*)&fat32_vfs_ops;
    n->flags = (e->attr & FAT_ATTR_DIR) ? VFS_DIRECTORY : VFS_FILE;
    return n;
}

/* finddir: find a child by name in the directory at parent->inode */
static vfs_node_t* _fat32_vfs_finddir(vfs_node_t* parent, const char* name)
{
    fat32_node_ctx_t* pctx = (fat32_node_ctx_t*)parent->impl;
    fat32_fs_t* fs = pctx->fs;
    if (!fs || !fs->valid) return NULL;

    uint32_t dir_cluster = (parent->inode >= 2) ? (uint32_t)parent->inode
                                                 : fs->root_cluster;
    fat32_entry_t entry;
    for (uint32_t i = 0; ; i++) {
        int r = fat32_read_dir_entry(fs, dir_cluster, i, &entry);
        if (r < 0) break;
        if (!entry.valid) continue;
        if (strncmp(entry.name, name, 255) == 0)
            return fat32_make_node(fs, &entry, dir_cluster, i);
    }
    return NULL;
}

/* Static dirent buffer for readdir (one at a time — single-threaded kernel) */
static vfs_dirent_t _fat32_dirent_buf;

/* readdir: return the idx-th valid entry from the directory at node->inode */
static vfs_dirent_t* _fat32_vfs_readdir_op(vfs_node_t* node, uint32_t idx)
{
    fat32_node_ctx_t* ctx = (fat32_node_ctx_t*)node->impl;
    fat32_fs_t* fs = ctx->fs;
    if (!fs || !fs->valid) return NULL;

    uint32_t dir_cluster = (node->inode >= 2) ? (uint32_t)node->inode
                                               : fs->root_cluster;
    uint32_t count = 0;
    fat32_entry_t entry;
    for (uint32_t i = 0; ; i++) {
        int r = fat32_read_dir_entry(fs, dir_cluster, i, &entry);
        if (r < 0) return NULL;
        if (!entry.valid) continue;
        if (count == idx) {
            strncpy(_fat32_dirent_buf.name, entry.name, VFS_NAME_MAX - 1);
            _fat32_dirent_buf.name[VFS_NAME_MAX - 1] = '\0';
            _fat32_dirent_buf.type = (entry.attr & FAT_ATTR_DIR)
                                     ? VFS_TYPE_DIR : VFS_TYPE_FILE;
            _fat32_dirent_buf.size = entry.size;
            return &_fat32_dirent_buf;
        }
        count++;
    }
}

/* read: read file data using node->inode as the first cluster */
static ssize_t _fat32_vfs_read(vfs_node_t* node, off_t offset,
                                size_t len, void* buf)
{
    fat32_node_ctx_t* ctx = (fat32_node_ctx_t*)node->impl;
    fat32_fs_t* fs = ctx->fs;
    if (!fs || !fs->valid) return -EIO;
    if (node->flags & VFS_DIRECTORY) return -EISDIR;

    uint32_t file_size = node->size;
    if ((uint64_t)offset >= file_size) return 0;
    if ((uint64_t)offset + len > file_size) len = file_size - (size_t)offset;
    if (len == 0) return 0;

    uint8_t* cbuf = (uint8_t*)kmalloc(fs->bytes_per_cluster);
    if (!cbuf) return -ENOMEM;

    ssize_t total = 0;
    uint32_t cluster_off = (uint32_t)((uint64_t)offset / fs->bytes_per_cluster);
    uint32_t byte_off    = (uint32_t)((uint64_t)offset % fs->bytes_per_cluster);

    /* Walk to starting cluster from node->inode (first cluster) */
    uint32_t c = (uint32_t)node->inode;
    for (uint32_t i = 0; i < cluster_off && c < FAT32_EOC_MIN; i++)
        c = fat32_next_cluster(fs, c);

    uint8_t* out = (uint8_t*)buf;
    while (total < (ssize_t)len && c >= 2 && c < FAT32_EOC_MIN) {
        if (fat32_read_cluster(fs, c, cbuf) != 0) { kfree(cbuf); return -EIO; }
        size_t avail = fs->bytes_per_cluster - byte_off;
        size_t copy  = MIN(avail, len - (size_t)total);
        memcpy(out + total, cbuf + byte_off, copy);
        total   += (ssize_t)copy;
        byte_off = 0;
        c = fat32_next_cluster(fs, c);
    }

    kfree(cbuf);
    return total;
}

static ssize_t _fat32_vfs_write(vfs_node_t* node, off_t offset,
                                  size_t len, const void* buf)
{
    fat32_node_ctx_t* ctx = (fat32_node_ctx_t*)node->impl;
    fat32_fs_t* fs = ctx->fs;
    if (!fs || !fs->valid) return -EIO;
    if (len == 0) return 0;
    if (node->flags & VFS_DIRECTORY) return -EISDIR;

    uint8_t* cbuf = (uint8_t*)kmalloc(fs->bytes_per_cluster);
    if (!cbuf) return -ENOMEM;

    /* Number of clusters needed to reach (offset + len) */
    uint64_t end_byte     = (uint64_t)offset + len;
    uint32_t cluster_off  = (uint32_t)((uint64_t)offset / fs->bytes_per_cluster);
    uint32_t byte_off     = (uint32_t)((uint64_t)offset % fs->bytes_per_cluster);

    /*
     * Walk (or extend) the cluster chain starting from node->inode.
     * If the file has no clusters yet, allocate the first one.
     */
    uint32_t c    = (uint32_t)node->inode;
    uint32_t prev = 0;

    if (c < 2) {
        /* Empty file — allocate first cluster */
        c = fat32_alloc_cluster(fs, 0);
        if (c < 2) { kfree(cbuf); return -ENOSPC; }
        node->inode = c;
    }

    /* Walk to cluster_off, allocating new clusters as needed */
    uint32_t ci = 0;
    while (ci < cluster_off) {
        prev = c;
        c = fat32_next_cluster(fs, c);
        if (c >= FAT32_EOC_MIN || c < 2) {
            /* Extend the chain */
            c = fat32_alloc_cluster(fs, prev);
            if (c < 2) { kfree(cbuf); return -ENOSPC; }
        }
        ci++;
    }

    /* Write loop */
    const uint8_t* src  = (const uint8_t*)buf;
    ssize_t        done = 0;

    while ((size_t)done < len) {
        /* Read-modify-write when the write doesn't start at cluster boundary
         * or doesn't fill the entire cluster */
        size_t avail = fs->bytes_per_cluster - byte_off;
        size_t copy  = MIN(avail, len - (size_t)done);

        if (byte_off != 0 || copy < fs->bytes_per_cluster) {
            /* Partial cluster: read first */
            if (fat32_read_cluster(fs, c, cbuf) != 0) {
                kfree(cbuf); return -EIO;
            }
        }

        memcpy(cbuf + byte_off, src + done, copy);

        if (fat32_write_cluster(fs, c, cbuf) != 0) {
            kfree(cbuf); return -EIO;
        }

        done     += (ssize_t)copy;
        byte_off  = 0;

        if ((size_t)done < len) {
            prev = c;
            c = fat32_next_cluster(fs, c);
            if (c >= FAT32_EOC_MIN || c < 2) {
                /* Extend chain */
                c = fat32_alloc_cluster(fs, prev);
                if (c < 2) break; /* Out of space — return partial write */
            }
        }
    }

    kfree(cbuf);

    /* Update the in-memory VFS node if the file grew */
    bool size_changed = (end_byte > (uint64_t)node->size);
    if (size_changed)
        node->size = (uint32_t)end_byte;

    /*
     * Write back the updated file size and first cluster to the on-disk
     * directory entry so the changes survive a remount.
     */
    if (ctx->dir_cluster != 0 && ctx->dirent_idx != (uint32_t)-1) {
        size_t   bpc  = fs->bytes_per_cluster;
        uint32_t epc  = bpc / 32;
        uint32_t ci   = ctx->dirent_idx / epc;
        uint32_t off  = ctx->dirent_idx % epc;
        uint32_t dc   = ctx->dir_cluster;
        for (uint32_t i = 0; i < ci; i++) {
            dc = fat32_next_cluster(fs, dc);
            if (dc >= FAT32_EOC_MIN || dc < 2) { dc = 0; break; }
        }
        if (dc >= 2) {
            uint8_t* dbuf = (uint8_t*)kmalloc(bpc);
            if (dbuf && fat32_read_cluster(fs, dc, dbuf) == 0) {
                fat32_dirent_t* de = (fat32_dirent_t*)(dbuf + off * 32);
                de->file_size  = (uint32_t)node->size;
                de->cluster_hi = (uint16_t)(node->inode >> 16);
                de->cluster_lo = (uint16_t)(node->inode & 0xFFFF);
                fat32_write_cluster(fs, dc, dbuf);
            }
            kfree(dbuf);
        }
    }
    fat32_sync(fs);

    return done;
}

/* ── Write ops: create, mkdir, unlink ───────────────────────────────── */

static int _fat32_vfs_create(vfs_node_t* parent_vnode, const char* name,
                              uint32_t mode)
{
    fat32_node_ctx_t* pctx = (fat32_node_ctx_t*)parent_vnode->impl;
    fat32_fs_t* fs = pctx->fs;
    (void)mode;

    uint32_t dir_cluster = (parent_vnode->inode >= 2)
                           ? (uint32_t)parent_vnode->inode : fs->root_cluster;

    int idx = fat32_find_free_dirent(fs, dir_cluster);
    if (idx < 0) return idx;

    fat32_dirent_t de;
    memset(&de, 0, sizeof(de));
    fat32_str_to_83(name, de.name, de.ext);
    de.attr = FAT_ATTR_ARCHIVE;  /* regular file */
    /* cluster_hi/lo and file_size all 0: empty file */

    int r = fat32_write_dirent_at(fs, dir_cluster, (uint32_t)idx, &de);
    if (r == 0) fat32_sync(fs);
    return r;
}

static int _fat32_vfs_mkdir_op(vfs_node_t* parent_vnode, const char* name,
                                uint32_t mode)
{
    fat32_node_ctx_t* pctx = (fat32_node_ctx_t*)parent_vnode->impl;
    fat32_fs_t* fs = pctx->fs;
    (void)mode;

    uint32_t parent_cluster = (parent_vnode->inode >= 2)
                              ? (uint32_t)parent_vnode->inode : fs->root_cluster;

    /* Allocate a cluster for the new directory */
    uint32_t new_cluster = fat32_alloc_cluster(fs, 0);
    if (new_cluster < 2) return -ENOSPC;

    /* Build the initial cluster: '.' and '..' entries */
    size_t   bpc  = fs->bytes_per_cluster;
    uint8_t* cbuf = (uint8_t*)kmalloc(bpc);
    if (!cbuf) { fat32_free_chain(fs, new_cluster); return -ENOMEM; }
    memset(cbuf, 0, bpc);

    fat32_dirent_t* dot = (fat32_dirent_t*)cbuf;
    memset(dot->name, ' ', 8); memset(dot->ext, ' ', 3);
    dot->name[0]   = '.';
    dot->attr      = FAT_ATTR_DIR;
    dot->cluster_hi = (uint16_t)(new_cluster >> 16);
    dot->cluster_lo = (uint16_t)(new_cluster & 0xFFFF);

    fat32_dirent_t* dotdot = (fat32_dirent_t*)(cbuf + 32);
    memset(dotdot->name, ' ', 8); memset(dotdot->ext, ' ', 3);
    dotdot->name[0] = '.'; dotdot->name[1] = '.';
    dotdot->attr    = FAT_ATTR_DIR;
    dotdot->cluster_hi = (uint16_t)(parent_cluster >> 16);
    dotdot->cluster_lo = (uint16_t)(parent_cluster & 0xFFFF);

    fat32_write_cluster(fs, new_cluster, cbuf);
    kfree(cbuf);

    /* Write the directory entry in the parent */
    int idx = fat32_find_free_dirent(fs, parent_cluster);
    if (idx < 0) { fat32_free_chain(fs, new_cluster); return idx; }

    fat32_dirent_t de;
    memset(&de, 0, sizeof(de));
    fat32_str_to_83(name, de.name, de.ext);
    de.attr      = FAT_ATTR_DIR;
    de.cluster_hi = (uint16_t)(new_cluster >> 16);
    de.cluster_lo = (uint16_t)(new_cluster & 0xFFFF);

    int r = fat32_write_dirent_at(fs, parent_cluster, (uint32_t)idx, &de);
    if (r != 0) { fat32_free_chain(fs, new_cluster); return r; }
    fat32_sync(fs);
    return 0;
}

static int _fat32_vfs_unlink(vfs_node_t* parent_vnode, const char* name)
{
    fat32_node_ctx_t* pctx = (fat32_node_ctx_t*)parent_vnode->impl;
    fat32_fs_t* fs = pctx->fs;

    uint32_t dir_cluster = (parent_vnode->inode >= 2)
                           ? (uint32_t)parent_vnode->inode : fs->root_cluster;

    size_t   bpc  = fs->bytes_per_cluster;
    uint32_t epc  = bpc / 32;
    uint8_t* cbuf = (uint8_t*)kmalloc(bpc);
    if (!cbuf) return -ENOMEM;

    uint32_t c    = dir_cluster;
    uint32_t cnum = 0;

    while (c >= 2 && c < FAT32_EOC_MIN) {
        if (fat32_read_cluster(fs, c, cbuf) != 0) { kfree(cbuf); return -EIO; }
        for (uint32_t i = 0; i < epc; i++) {
            fat32_dirent_t* de = (fat32_dirent_t*)(cbuf + i * 32);
            if (de->name[0] == 0x00) { kfree(cbuf); return -ENOENT; }
            if ((uint8_t)de->name[0] == 0xE5) continue;
            if (de->attr == FAT_ATTR_LFN) continue;
            if (de->attr & (FAT_ATTR_SYSTEM | FAT_ATTR_VOLUME_ID)) continue;

            char fname[13];
            fat32_83_to_str(de, fname);
            if (strcmp(fname, name) == 0) {
                uint32_t fc = ((uint32_t)de->cluster_hi << 16) | de->cluster_lo;
                if (fc >= 2) fat32_free_chain(fs, fc);
                de->name[0] = (uint8_t)0xE5;  /* mark deleted */
                fat32_write_cluster(fs, c, cbuf);
                kfree(cbuf);
                fat32_sync(fs);
                return 0;
            }
        }
        c = fat32_next_cluster(fs, c);
        cnum++;
    }

    kfree(cbuf);
    return -ENOENT;
}

/* ── Mount ───────────────────────────────────────────────────────────── */

int fat32_init(fat32_fs_t* fs, int dev, int lba_start, const char* mount_point)
{
    memset(fs, 0, sizeof(*fs));
    fs->dev                = dev;
    fs->partition_start_lba = lba_start;
    fs->sector_buf_lba     = 0xFFFFFFFFu;

    /* Read BPB from sector 0 */
    if (fat32_read_sector(fs, 0, &fs->bpb) != 0) {
        kerror("FAT32: cannot read BPB from dev %d", dev);
        return -EIO;
    }

    /* Validate */
    fat32_bpb_t* b = &fs->bpb;
    if (b->bytes_per_sector != 512) {
        kerror("FAT32: unsupported sector size %u", b->bytes_per_sector);
        return -EINVAL;
    }
    if (b->num_fats < 1) return -EINVAL;

    /* Derived geometry */
    fs->fat_lba              = b->reserved_sectors;
    fs->data_lba             = b->reserved_sectors +
                               b->num_fats * b->fat_size_32;
    fs->root_cluster         = b->root_cluster;
    fs->sectors_per_cluster  = b->sectors_per_cluster;
    fs->bytes_per_cluster    = b->sectors_per_cluster * 512;
    fs->total_clusters       = (b->total_sectors_32 - fs->data_lba) /
                               b->sectors_per_cluster;
    fs->free_clusters        = 0xFFFFFFFFu;
    fs->next_free_hint       = 2;

    /* Try to read FSInfo */
    uint8_t fsinfo_buf[512];
    if (fat32_read_sector(fs, b->fs_info_sector, fsinfo_buf) == 0) {
        fat32_fsinfo_t* fi = (fat32_fsinfo_t*)fsinfo_buf;
        if (fi->lead_sig == 0x41615252u && fi->struct_sig == 0x61417272u) {
            fs->free_clusters   = fi->free_count;
            fs->next_free_hint  = fi->next_free;
        }
    }

    strncpy(fs->mount_point, mount_point, 255);
    fs->valid = true;

    kinfo("FAT32: mounted dev %d at '%s' (%u clusters, %u bytes/cluster)",
          dev, mount_point, fs->total_clusters, fs->bytes_per_cluster);

    /* Create a VFS root node for this filesystem and register it */
    fat32_node_ctx_t* root_ctx = (fat32_node_ctx_t*)kmalloc(sizeof(fat32_node_ctx_t));
    vfs_node_t*       root     = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));
    if (!root_ctx || !root) { kfree(root_ctx); kfree(root); return -ENOMEM; }
    memset(root,     0, sizeof(*root));
    memset(root_ctx, 0, sizeof(*root_ctx));

    root_ctx->fs          = fs;
    root_ctx->dir_cluster = fs->root_cluster; /* parent of root is itself */
    root_ctx->dirent_idx  = (uint32_t)-1;     /* no directory entry to write back */

    strncpy(root->name, "/", VFS_NAME_MAX);
    root->flags = VFS_DIRECTORY;
    root->inode = 0;          /* 0 triggers use of fs->root_cluster in ops */
    root->impl  = (void*)root_ctx;
    root->ops   = (vfs_ops_t*)&fat32_vfs_ops;

    vfs_mount(mount_point, root, fs);

    return 0;
}

int fat32_sync(fat32_fs_t* fs)
{
    if (!fs || !fs->valid) return -EINVAL;
    if (fs->sector_buf_dirty) {
        fat32_write_sector(fs, fs->sector_buf_lba, fs->sector_buf);
        fs->sector_buf_dirty = false;
    }
    return 0;
}

void fat32_unmount(fat32_fs_t* fs)
{
    if (!fs || !fs->valid) return;
    fat32_sync(fs);
    fs->valid = false;
    kinfo("FAT32: unmounted '%s'", fs->mount_point);
}

/* ── Auto-detect and mount ───────────────────────────────────────────── */

int fat32_mount_auto(void)
{
    /* Try to mount from ATA device 0, partition starting at LBA 0 */
    for (int i = 0; i < FAT32_MAX_MOUNTS; i++) {
        if (fat32_mounts[i].valid) continue;
        int r = fat32_init(&fat32_mounts[i], 0, 0, "/mnt");
        if (r == 0) return 0;
        break;
    }
    return -ENODEV;
}

/*
 * fat32_mount - convenience wrapper used by diskman.
 * Allocates the next free mount-table slot and calls fat32_init.
 */
int fat32_mount(int drive, uint64_t lba_start, const char* mount_point)
{
    for (int i = 0; i < FAT32_MAX_MOUNTS; i++) {
        if (fat32_mounts[i].valid) continue;
        return fat32_init(&fat32_mounts[i], drive,
                          (int)(uint32_t)lba_start, mount_point);
    }
    kerror("FAT32: mount table full");
    return -ENOMEM;
}
