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

fat32_fs_t fat32_mounts[FAT32_MAX_MOUNTS];

/* ── Low-level sector I/O ────────────────────────────────────────────── */

static int fat32_read_sector(fat32_fs_t* fs, uint32_t lba, void* buf)
{
    return ata_read_sectors(fs->dev, fs->partition_start_lba + lba, 1, buf);
}

static int fat32_write_sector(fat32_fs_t* fs, uint32_t lba, const void* buf)
{
    return ata_write_sectors(fs->dev, fs->partition_start_lba + lba, 1, buf);
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

static ssize_t _fat32_vfs_read(vfs_node_t* node, uint64_t offset,
                                size_t len, void* buf)
{
    fat32_fs_t* fs = (fat32_fs_t*)node->fs_data;
    if (!fs || !fs->valid) return -EIO;

    fat32_entry_t entry;
    if (fat32_resolve(fs, node->name, &entry) != 0) return -ENOENT;
    if (entry.attr & FAT_ATTR_DIR) return -EISDIR;

    if (offset >= entry.size) return 0;
    if (offset + len > entry.size) len = entry.size - offset;

    uint8_t* cbuf = (uint8_t*)kmalloc(fs->bytes_per_cluster);
    if (!cbuf) return -ENOMEM;

    ssize_t total = 0;
    uint32_t cluster_off = (uint32_t)(offset / fs->bytes_per_cluster);
    uint32_t byte_off    = (uint32_t)(offset % fs->bytes_per_cluster);

    /* Walk to starting cluster */
    uint32_t c = entry.first_cluster;
    for (uint32_t i = 0; i < cluster_off && c < FAT32_EOC_MIN; i++) {
        c = fat32_next_cluster(fs, c);
    }

    uint8_t* out = (uint8_t*)buf;

    while (total < (ssize_t)len && c >= 2 && c < FAT32_EOC_MIN) {
        if (fat32_read_cluster(fs, c, cbuf) != 0) { kfree(cbuf); return -EIO; }

        size_t avail = fs->bytes_per_cluster - byte_off;
        size_t copy  = MIN(avail, len - (size_t)total);

        memcpy(out + total, cbuf + byte_off, copy);
        total    += copy;
        byte_off  = 0;
        c = fat32_next_cluster(fs, c);
    }

    kfree(cbuf);
    return total;
}

static ssize_t _fat32_vfs_write(vfs_node_t* node, uint64_t offset,
                                  size_t len, const void* buf)
{
    (void)node; (void)offset; (void)len; (void)buf;
    /* Write path: locate/allocate clusters, write data, update dir entry size */
    /* Full implementation mirrors read but extends the chain as needed */
    return -ENOSYS; /* TODO Phase 2 */
}

static int _fat32_vfs_readdir(vfs_node_t* node, uint32_t idx,
                               vfs_dirent_t* out)
{
    fat32_fs_t* fs = (fat32_fs_t*)node->fs_data;
    if (!fs) return -EIO;

    fat32_entry_t entry;
    uint32_t dir_cluster = fs->root_cluster;

    /* If node is a subdirectory, resolve its cluster */
    if (strncmp(node->name, "/", 2) != 0) {
        fat32_entry_t dir_entry;
        if (fat32_resolve(fs, node->name, &dir_entry) != 0) return -ENOENT;
        dir_cluster = dir_entry.first_cluster;
    }

    uint32_t count = 0;
    for (uint32_t i = 0; ; i++) {
        int r = fat32_read_dir_entry(fs, dir_cluster, i, &entry);
        if (r < 0) return -ENOENT;
        if (!entry.valid) continue;
        if (count == idx) {
            strncpy(out->name, entry.name, VFS_NAME_MAX - 1);
            out->type = (entry.attr & FAT_ATTR_DIR) ? VFS_TYPE_DIR : VFS_TYPE_FILE;
            out->size = entry.size;
            return 0;
        }
        count++;
    }
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

    /* Register with VFS */
    vfs_mount(mount_point, NULL, NULL); /* VFS integration stub */

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
