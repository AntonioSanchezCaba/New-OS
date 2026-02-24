/*
 * kernel/pkg.c - AetherOS Package Manager (APM)
 *
 * Manages installation and removal of .aur package archives.
 * The package database is stored as a binary blob at /sys/packages/db.
 */
#include <kernel/pkg.h>
#include <fs/vfs.h>
#include <memory.h>
#include <kernel.h>
#include <string.h>
#include <drivers/timer.h>
#include <process.h>

pkg_db_t g_pkg_db;

/* ── CRC32 (ISO 3309 polynomial 0xEDB88320) ──────────────────────────── */

uint32_t pkg_crc32(const void* data, size_t len)
{
    static uint32_t table[256];
    static bool     table_ready = false;

    if (!table_ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int j = 0; j < 8; j++)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        table_ready = true;
    }

    const uint8_t* buf = (const uint8_t*)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        crc = table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

/* ── Database persistence ────────────────────────────────────────────── */

void pkg_init(void)
{
    memset(&g_pkg_db, 0, sizeof(g_pkg_db));

    /* Try to load database from disk */
    vfs_node_t* node = vfs_open(PKG_DB_PATH, 0);
    if (!node) {
        kinfo("PKG: no database found at %s, starting fresh", PKG_DB_PATH);
        return;
    }

    vfs_read(node, 0, sizeof(g_pkg_db), &g_pkg_db);
    vfs_close(node);

    kinfo("PKG: loaded database: %u package(s) installed", g_pkg_db.count);
}

void pkg_db_save(void)
{
    /* Ensure /sys/packages/ exists */
    vfs_mkdir("/sys");
    vfs_mkdir("/sys/packages");

    vfs_node_t* node = vfs_open(PKG_DB_PATH, VFS_O_CREAT | VFS_O_WRONLY);
    if (!node) {
        kerror("PKG: cannot write database to %s", PKG_DB_PATH);
        return;
    }

    vfs_write(node, 0, sizeof(g_pkg_db), &g_pkg_db);
    vfs_close(node);
    kdebug("PKG: database saved (%u packages)", g_pkg_db.count);
}

/* ── Query ───────────────────────────────────────────────────────────── */

pkg_entry_t* pkg_find(const char* name)
{
    for (uint32_t i = 0; i < PKG_MAX_INSTALLED; i++) {
        if (g_pkg_db.packages[i].valid &&
            strncmp(g_pkg_db.packages[i].name, name, PKG_NAME_MAX) == 0) {
            return &g_pkg_db.packages[i];
        }
    }
    return NULL;
}

bool pkg_installed(const char* name)
{
    return pkg_find(name) != NULL;
}

int pkg_list(pkg_entry_t** out, int max)
{
    int count = 0;
    for (int i = 0; i < PKG_MAX_INSTALLED && count < max; i++) {
        if (g_pkg_db.packages[i].valid) {
            if (out) out[count] = &g_pkg_db.packages[i];
            count++;
        }
    }
    return count;
}

/* ── Install ─────────────────────────────────────────────────────────── */

int pkg_install(const char* aur_path)
{
    if (!aur_path) return -EINVAL;

    kinfo("PKG: installing from '%s'", aur_path);

    /* Open the archive */
    vfs_node_t* node = vfs_open(aur_path, 0);
    if (!node) {
        kerror("PKG: cannot open '%s'", aur_path);
        return -ENOENT;
    }

    /* Read header */
    pkg_header_t hdr;
    ssize_t n = vfs_read(node, 0, sizeof(hdr), &hdr);
    if (n < (ssize_t)sizeof(hdr)) {
        vfs_close(node);
        return -EIO;
    }

    if (hdr.magic != PKG_MAGIC) {
        kerror("PKG: '%s' is not a valid .aur package", aur_path);
        vfs_close(node);
        return -EINVAL;
    }

    /* Check if already installed */
    if (pkg_installed(hdr.name)) {
        kwarn("PKG: '%s' is already installed", hdr.name);
        vfs_close(node);
        return -EEXIST;
    }

    /* Verify checksum */
    if (pkg_verify(aur_path) != 0) {
        kerror("PKG: checksum failed for '%s'", aur_path);
        vfs_close(node);
        return -EINVAL;
    }

    /* Read file entries */
    pkg_file_entry_t file_entries[PKG_MAX_FILES];
    size_t file_table_size = hdr.num_files * sizeof(pkg_file_entry_t);
    if (hdr.num_files > PKG_MAX_FILES) {
        kerror("PKG: too many files in '%s'", hdr.name);
        vfs_close(node);
        return -EINVAL;
    }

    uint64_t file_table_off = sizeof(pkg_header_t);
    n = vfs_read(node, file_table_off, file_table_size, file_entries);
    if (n < (ssize_t)file_table_size) {
        vfs_close(node);
        return -EIO;
    }

    /* Extract files */
    uint8_t* iobuf = (uint8_t*)kmalloc(4096);
    if (!iobuf) { vfs_close(node); return -ENOMEM; }

    for (uint32_t i = 0; i < hdr.num_files; i++) {
        pkg_file_entry_t* fe = &file_entries[i];

        if (fe->type == PKG_FILE_DIR) {
            vfs_mkdir(fe->path);
            continue;
        }

        /* Regular file: create and write */
        vfs_node_t* dst = vfs_open(fe->path, VFS_O_CREAT | VFS_O_WRONLY);
        if (!dst) {
            kwarn("PKG: cannot create '%s'", fe->path);
            continue;
        }

        uint64_t src_off  = hdr.payload_offset + fe->offset;
        uint64_t dst_off  = 0;
        uint32_t remaining = fe->size;

        while (remaining > 0) {
            size_t chunk = MIN(4096u, remaining);
            n = vfs_read(node, src_off, chunk, iobuf);
            if (n <= 0) break;
            vfs_write(dst, dst_off, (size_t)n, iobuf);
            src_off    += n;
            dst_off    += n;
            remaining  -= n;
        }

        vfs_close(dst);
    }

    kfree(iobuf);
    vfs_close(node);

    /* Register in database */
    pkg_entry_t* entry = NULL;
    for (int i = 0; i < PKG_MAX_INSTALLED; i++) {
        if (!g_pkg_db.packages[i].valid) {
            entry = &g_pkg_db.packages[i];
            break;
        }
    }

    if (!entry) {
        kerror("PKG: database full, cannot register '%s'", hdr.name);
        return -ENOSPC;
    }

    memset(entry, 0, sizeof(*entry));
    entry->valid = true;
    strncpy(entry->name,        hdr.name,        PKG_NAME_MAX - 1);
    strncpy(entry->version,     hdr.version,     PKG_VER_MAX - 1);
    strncpy(entry->description, hdr.description, PKG_DESC_MAX - 1);
    entry->num_files    = hdr.num_files;
    entry->install_time = timer_get_ticks();

    for (uint32_t i = 0; i < hdr.num_files && i < PKG_MAX_FILES; i++) {
        strncpy(entry->files[i], file_entries[i].path, 255);
    }

    g_pkg_db.count++;
    pkg_db_save();

    kinfo("PKG: installed '%s' v%s (%u files)",
          hdr.name, hdr.version, hdr.num_files);
    return 0;
}

/* ── Uninstall ───────────────────────────────────────────────────────── */

int pkg_uninstall(const char* name)
{
    pkg_entry_t* entry = pkg_find(name);
    if (!entry) {
        kerror("PKG: '%s' is not installed", name);
        return -ENOENT;
    }

    kinfo("PKG: removing '%s' (%u files)", name, entry->num_files);

    /* Remove all installed files */
    for (uint32_t i = 0; i < entry->num_files; i++) {
        if (entry->files[i][0]) {
            int r = vfs_unlink(entry->files[i]);
            if (r != 0) kwarn("PKG: cannot remove '%s'", entry->files[i]);
        }
    }

    entry->valid = false;
    if (g_pkg_db.count > 0) g_pkg_db.count--;

    pkg_db_save();
    kinfo("PKG: '%s' removed", name);
    return 0;
}

/* ── Verify ──────────────────────────────────────────────────────────── */

int pkg_verify(const char* aur_path)
{
    vfs_node_t* node = vfs_open(aur_path, 0);
    if (!node) return -ENOENT;

    pkg_header_t hdr;
    vfs_read(node, 0, sizeof(hdr), &hdr);

    /* Read entire payload and compute CRC32 */
    uint8_t* payload = (uint8_t*)kmalloc(hdr.payload_size);
    if (!payload) { vfs_close(node); return -ENOMEM; }

    ssize_t n = vfs_read(node, hdr.payload_offset, hdr.payload_size, payload);
    vfs_close(node);

    if (n < (ssize_t)hdr.payload_size) {
        kfree(payload);
        return -EIO;
    }

    uint32_t crc = pkg_crc32(payload, hdr.payload_size);
    kfree(payload);

    return (crc == hdr.checksum) ? 0 : -EINVAL;
}

/* ── Build a package (dev tool) ──────────────────────────────────────── */

int pkg_build(const char* src_dir, const char* pkg_name,
               const char* version, const char* out_path)
{
    if (!src_dir || !pkg_name || !version || !out_path) return -EINVAL;

    kinfo("PKG: building '%s' v%s from '%s' → '%s'",
          pkg_name, version, src_dir, out_path);

    /* ── Step 1: scan src_dir and collect file entries ───────────────── */
    pkg_file_entry_t* file_table =
        (pkg_file_entry_t*)kmalloc(PKG_MAX_FILES * sizeof(pkg_file_entry_t));
    if (!file_table) return -ENOMEM;

    uint32_t file_count   = 0;
    uint32_t payload_size = 0;

    vfs_node_t* dir = vfs_open(src_dir, 0);
    if (!dir) {
        kfree(file_table);
        kerror("PKG: cannot open src dir '%s'", src_dir);
        return -ENOENT;
    }

    for (uint32_t idx = 0; file_count < PKG_MAX_FILES; idx++) {
        vfs_dirent_t de;
        if (vfs_readdir(dir, idx, &de) != 0) break;
        if (de.name[0] == '.') continue;  /* skip . and .. */

        pkg_file_entry_t* fe = &file_table[file_count];
        memset(fe, 0, sizeof(*fe));

        /* Build full install path: src_dir + '/' + name */
        size_t dlen = strlen(src_dir);
        size_t nlen = strlen(de.name);
        if (dlen + 1 + nlen >= sizeof(fe->path)) continue;
        memcpy(fe->path, src_dir, dlen);
        fe->path[dlen] = '/';
        memcpy(fe->path + dlen + 1, de.name, nlen + 1);

        fe->offset = payload_size;
        fe->size   = (uint32_t)de.size;
        fe->mode   = 0644;
        fe->type   = (de.type == VFS_TYPE_DIR) ? PKG_FILE_DIR : PKG_FILE_REG;

        if (fe->type == PKG_FILE_REG)
            payload_size += fe->size;

        file_count++;
    }
    vfs_close(dir);

    /* ── Step 2: read file contents into a payload buffer ────────────── */
    uint8_t* payload = NULL;
    if (payload_size > 0) {
        payload = (uint8_t*)kmalloc(payload_size);
        if (!payload) { kfree(file_table); return -ENOMEM; }

        uint32_t woff = 0;
        for (uint32_t i = 0; i < file_count; i++) {
            if (file_table[i].type != PKG_FILE_REG || file_table[i].size == 0)
                continue;
            vfs_node_t* fn = vfs_open(file_table[i].path, 0);
            if (!fn) { woff += file_table[i].size; continue; }
            ssize_t n = vfs_read(fn, 0, file_table[i].size, payload + woff);
            vfs_close(fn);
            if (n > 0) woff += (uint32_t)n;
            else woff += file_table[i].size; /* pad zeros for unreadable files */
        }
    }

    uint32_t crc = payload_size ? pkg_crc32(payload, payload_size) : 0;

    /* ── Step 3: assemble header ─────────────────────────────────────── */
    pkg_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic          = PKG_MAGIC;
    hdr.format_version = PKG_VERSION;
    strncpy(hdr.name,    pkg_name, PKG_NAME_MAX - 1);
    strncpy(hdr.version, version,  PKG_VER_MAX  - 1);
    hdr.num_files      = file_count;
    hdr.payload_offset = (uint32_t)(sizeof(hdr) +
                                    file_count * sizeof(pkg_file_entry_t));
    hdr.payload_size   = payload_size;
    hdr.checksum       = crc;

    /* ── Step 4: write the .aur archive ──────────────────────────────── */
    vfs_node_t* out = vfs_open(out_path,
                                VFS_O_CREAT | VFS_O_WRONLY | O_TRUNC);
    if (!out) {
        if (payload) kfree(payload);
        kfree(file_table);
        kerror("PKG: cannot create '%s'", out_path);
        return -EACCES;
    }

    off_t woff = 0;
    vfs_write(out, woff, sizeof(hdr), &hdr);
    woff += (off_t)sizeof(hdr);

    size_t tbl_sz = file_count * sizeof(pkg_file_entry_t);
    vfs_write(out, woff, tbl_sz, file_table);
    woff += (off_t)tbl_sz;

    if (payload_size && payload)
        vfs_write(out, woff, payload_size, payload);

    vfs_close(out);
    if (payload) kfree(payload);
    kfree(file_table);

    kinfo("PKG: built '%s' v%s — %u files, %u bytes payload → '%s'",
          pkg_name, version, file_count, payload_size, out_path);
    return 0;
}

/* ── Syscalls ────────────────────────────────────────────────────────── */

int64_t sys_pkg_install(uint64_t path_addr, uint64_t a2, uint64_t a3,
                         uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    const char* path = (const char*)path_addr;
    if (!path) return -EFAULT;
    return pkg_install(path);
}

int64_t sys_pkg_remove(uint64_t name_addr, uint64_t a2, uint64_t a3,
                        uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    const char* name = (const char*)name_addr;
    if (!name) return -EFAULT;
    return pkg_uninstall(name);
}

int64_t sys_pkg_list(uint64_t buf_addr, uint64_t max, uint64_t a3,
                      uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (!buf_addr || max == 0) return -EINVAL;

    pkg_entry_t** out = (pkg_entry_t**)buf_addr;
    return pkg_list(out, (int)max);
}
