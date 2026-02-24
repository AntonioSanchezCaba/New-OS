/*
 * kernel/apkg.c — AetherOS Package Format (.apkg) implementation
 *
 * Manages installed package records in an in-memory database (max 64 entries).
 * Validates package headers, checks dependencies, verifies CRC32, and records
 * installs.  Persists the DB as a plain-text CSV via VFS at /sys/pkg/db.
 *
 * The payload execution model for this milestone:
 *   In-kernel apps are compiled directly into the kernel binary, so payload
 *   bytes are not executed here.  apkg_install() validates, records, and
 *   returns success so the GUI package manager can track state.  A future
 *   user-space ELF loader will execute ring-3 payloads.
 */
#include <kernel/apkg.h>
#include <fs/vfs.h>
#include <memory.h>
#include <string.h>
#include <kernel.h>

/* =========================================================
 * CRC32 (ISO 3309 / Ethernet polynomial 0xEDB88320)
 * ========================================================= */
uint32_t apkg_crc32(const void* data, size_t len)
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

    const uint8_t* p = (const uint8_t*)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

/* =========================================================
 * Package database
 * ========================================================= */
static apkg_record_t g_db[APKG_DB_MAX];
static int           g_db_count = 0;
static bool          g_initialized = false;

/* Package catalog (available but not yet installed) */
static apkg_catalog_t g_catalog[APKG_CATALOG_MAX];
static int            g_catalog_count = 0;

#define APKG_DB_VFS_PATH  "/sys/pkg/db"

void apkg_init(void)
{
    memset(g_db,      0, sizeof(g_db));
    memset(g_catalog, 0, sizeof(g_catalog));
    g_db_count      = 0;
    g_catalog_count = 0;
    g_initialized   = true;
    apkg_load();
    apkg_repo_scan("/sys/packages/cache");
    kinfo("APKG: initialized — %d installed, %d available",
          g_db_count, g_catalog_count);
}

/* =========================================================
 * Repository / catalog
 * ========================================================= */

/*
 * apkg_repo_scan — walk a VFS directory for .apkg files.
 * Reads each file's 256-byte header to extract metadata and
 * appends new entries to g_catalog[].  Safe to call multiple
 * times with different directories.
 * Returns the number of new catalog entries added.
 */
int apkg_repo_scan(const char* dir_path)
{
    if (!dir_path) return 0;

    vfs_node_t* dir = vfs_open(dir_path, VFS_O_READ);
    if (!dir) return 0;

    int added = 0;
    uint32_t idx = 0;
    vfs_dirent_t* de;

    while ((de = vfs_readdir(dir, idx++)) != NULL) {
        /* Only process names ending in ".apkg" */
        size_t nlen = strlen(de->name);
        if (nlen < 6 ||
            strcmp(de->name + nlen - 5, ".apkg") != 0)
            continue;

        if (g_catalog_count >= APKG_CATALOG_MAX)
            break;

        /* Build full path */
        char fpath[APKG_PATH_LEN];
        size_t dlen = strlen(dir_path);
        if (dlen + 1 + nlen + 1 > APKG_PATH_LEN)
            continue;
        memcpy(fpath, dir_path, dlen);
        fpath[dlen] = '/';
        memcpy(fpath + dlen + 1, de->name, nlen + 1);

        /* Open and read the 256-byte header */
        vfs_node_t* fnode = vfs_open(fpath, VFS_O_READ);
        if (!fnode) continue;

        apkg_header_t hdr;
        ssize_t nr = vfs_read(fnode, 0, sizeof(hdr), &hdr);
        vfs_close(fnode);

        if (nr < (ssize_t)sizeof(hdr)) continue;
        if (memcmp(hdr.magic, APKG_MAGIC, APKG_MAGIC_LEN) != 0) continue;
        if (hdr.fmt_version != APKG_FMT_VERSION) continue;

        /* Skip if already in catalog (same name) */
        bool dup = false;
        for (int c = 0; c < g_catalog_count; c++) {
            if (strncmp(g_catalog[c].name, hdr.name, APKG_NAME_LEN) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) continue;

        apkg_catalog_t* ce = &g_catalog[g_catalog_count++];
        memset(ce, 0, sizeof(*ce));
        strncpy(ce->name,        hdr.name,        APKG_NAME_LEN  - 1);
        strncpy(ce->version,     hdr.version,     APKG_VER_LEN   - 1);
        strncpy(ce->description, hdr.description, APKG_DESC_LEN  - 1);
        strncpy(ce->author,      hdr.author,      APKG_AUTHOR_LEN - 1);
        strncpy(ce->path,        fpath,           APKG_PATH_LEN  - 1);
        ce->available = true;
        added++;
    }

    vfs_close(dir);
    if (added > 0)
        kinfo("APKG: repo scan '%s' — %d new package(s)", dir_path, added);
    return added;
}

/*
 * apkg_repo_install_by_name — find a package in the catalog,
 * load its .apkg file into a heap buffer, and call apkg_install().
 * Returns 0 on success, negative on error (same codes as apkg_install()).
 */
int apkg_repo_install_by_name(const char* name)
{
    if (!name) return -1;

    /* Find in catalog */
    apkg_catalog_t* ce = NULL;
    for (int i = 0; i < g_catalog_count; i++) {
        if (strncmp(g_catalog[i].name, name, APKG_NAME_LEN) == 0 &&
            g_catalog[i].available) {
            ce = &g_catalog[i];
            break;
        }
    }
    if (!ce) {
        klog_warn("APKG: '%s' not found in catalog", name);
        return -1;
    }

    /* Open the .apkg file */
    vfs_node_t* node = vfs_open(ce->path, VFS_O_READ);
    if (!node) {
        klog_warn("APKG: cannot open '%s'", ce->path);
        return -1;
    }

    /* Determine file size by reading in chunks */
    size_t file_size = 0;
    {
        char probe[512];
        ssize_t r;
        while ((r = vfs_read(node, (off_t)file_size, sizeof(probe), probe)) > 0)
            file_size += (size_t)r;
    }

    if (file_size < sizeof(apkg_header_t)) {
        vfs_close(node);
        return -1;
    }

    uint8_t* buf = (uint8_t*)kmalloc(file_size);
    if (!buf) {
        vfs_close(node);
        return -1;
    }

    ssize_t nr = vfs_read(node, 0, file_size, buf);
    vfs_close(node);

    if (nr < (ssize_t)file_size) {
        kfree(buf);
        return -1;
    }

    int rc = apkg_install(buf, file_size);
    kfree(buf);
    return rc;
}

int apkg_catalog_count(void) { return g_catalog_count; }

const apkg_catalog_t* apkg_catalog_get(int idx)
{
    if (idx < 0 || idx >= g_catalog_count) return NULL;
    return &g_catalog[idx];
}

/* =========================================================
 * Helpers
 * ========================================================= */
static int find_installed(const char* name)
{
    for (int i = 0; i < g_db_count; i++)
        if (g_db[i].installed && strncmp(g_db[i].name, name, APKG_NAME_LEN) == 0)
            return i;
    return -1;
}

/* Simple semver compare: returns <0 if a<b, 0 if equal, >0 if a>b.
 * Handles "MAJOR.MINOR.PATCH" format; stops at first differing component. */
static int semver_cmp(const char* a, const char* b)
{
    /* Parse each numeric component */
    for (int part = 0; part < 3; part++) {
        int va = 0, vb = 0;
        while (*a >= '0' && *a <= '9') { va = va * 10 + (*a - '0'); a++; }
        while (*b >= '0' && *b <= '9') { vb = vb * 10 + (*b - '0'); b++; }
        if (va != vb) return va - vb;
        if (*a == '.') a++;
        if (*b == '.') b++;
    }
    return 0;
}

/* =========================================================
 * Install
 * ========================================================= */
int apkg_install(const uint8_t* pkg_data, size_t pkg_size)
{
    if (!pkg_data || pkg_size < sizeof(apkg_header_t))
        return -1;

    const apkg_header_t* hdr = (const apkg_header_t*)pkg_data;

    /* Validate magic */
    if (memcmp(hdr->magic, APKG_MAGIC, APKG_MAGIC_LEN) != 0) {
        klog_warn("APKG: bad magic");
        return -1;
    }
    if (hdr->fmt_version != APKG_FMT_VERSION) {
        klog_warn("APKG: unsupported format version %u", hdr->fmt_version);
        return -1;
    }

    /* Already installed? */
    if (find_installed(hdr->name) >= 0) {
        klog_warn("APKG: '%s' already installed", hdr->name);
        return -5;
    }

    /* DB full? */
    if (g_db_count >= APKG_DB_MAX) {
        klog_warn("APKG: package DB full");
        return -4;
    }

    /* Locate dependency table and payload */
    size_t dep_table_off = sizeof(apkg_header_t);
    size_t dep_table_sz  = (size_t)hdr->dep_count * sizeof(apkg_dep_t);
    size_t payload_off   = dep_table_off + dep_table_sz;

    if (payload_off + hdr->payload_size > pkg_size) {
        klog_warn("APKG: '%s' truncated (expected %zu bytes, got %zu)",
                  hdr->name, payload_off + hdr->payload_size, pkg_size);
        return -1;
    }

    /* CRC32 check */
    if (hdr->payload_size > 0) {
        uint32_t computed = apkg_crc32(pkg_data + payload_off, hdr->payload_size);
        if (computed != hdr->payload_crc32) {
            klog_warn("APKG: '%s' CRC32 mismatch (got %08X, expected %08X)",
                      hdr->name, computed, hdr->payload_crc32);
            return -2;
        }
    }

    /* Dependency check */
    if (hdr->dep_count > 0) {
        const apkg_dep_t* deps =
            (const apkg_dep_t*)(pkg_data + dep_table_off);
        for (int d = 0; d < (int)hdr->dep_count; d++) {
            int idx = find_installed(deps[d].name);
            if (idx < 0) {
                klog_warn("APKG: '%s' requires '%s' (not installed)",
                          hdr->name, deps[d].name);
                return -3;
            }
            /* Version check */
            if (deps[d].min_ver[0] &&
                semver_cmp(g_db[idx].version, deps[d].min_ver) < 0) {
                klog_warn("APKG: '%s' requires '%s' >= '%s' (have '%s')",
                          hdr->name, deps[d].name,
                          deps[d].min_ver, g_db[idx].version);
                return -3;
            }
        }
    }

    /* Record installation */
    apkg_record_t* rec = &g_db[g_db_count++];
    memset(rec, 0, sizeof(*rec));
    strncpy(rec->name,        hdr->name,        APKG_NAME_LEN - 1);
    strncpy(rec->version,     hdr->version,     APKG_VER_LEN  - 1);
    strncpy(rec->description, hdr->description, APKG_DESC_LEN - 1);
    strncpy(rec->author,      hdr->author,      APKG_AUTHOR_LEN - 1);
    rec->installed = true;

    kinfo("APKG: installed '%s' v%s by %s (%u bytes payload)",
          rec->name, rec->version, rec->author, hdr->payload_size);

    apkg_save();
    return 0;
}

/* =========================================================
 * Remove
 * ========================================================= */
int apkg_remove(const char* name)
{
    int idx = find_installed(name);
    if (idx < 0) return -1;

    /* Shift remaining entries down */
    for (int i = idx; i < g_db_count - 1; i++)
        g_db[i] = g_db[i + 1];
    g_db_count--;
    memset(&g_db[g_db_count], 0, sizeof(apkg_record_t));

    kinfo("APKG: removed '%s'", name);
    apkg_save();
    return 0;
}

/* =========================================================
 * Query
 * ========================================================= */
const apkg_record_t* apkg_find(const char* name)
{
    int idx = find_installed(name);
    return (idx >= 0) ? &g_db[idx] : NULL;
}

int apkg_count(void) { return g_db_count; }

const apkg_record_t* apkg_get(int idx)
{
    if (idx < 0 || idx >= g_db_count) return NULL;
    return &g_db[idx];
}

/* =========================================================
 * Persistence  (CSV: name,version,author,description\n)
 * ========================================================= */
void apkg_save(void)
{
    /* Build text blob */
    char buf[APKG_DB_MAX * (APKG_NAME_LEN + APKG_VER_LEN + APKG_AUTHOR_LEN + 4)];
    int  pos = 0;
    int  cap = (int)sizeof(buf) - 1;

    for (int i = 0; i < g_db_count && pos < cap - 8; i++) {
        const apkg_record_t* r = &g_db[i];
        /* name */
        int n = (int)strlen(r->name);
        if (pos + n + 1 > cap) break;
        memcpy(buf + pos, r->name, (size_t)n); pos += n;
        buf[pos++] = ',';
        /* version */
        n = (int)strlen(r->version);
        if (pos + n + 1 > cap) break;
        memcpy(buf + pos, r->version, (size_t)n); pos += n;
        buf[pos++] = ',';
        /* author */
        n = (int)strlen(r->author);
        if (pos + n + 1 > cap) break;
        memcpy(buf + pos, r->author, (size_t)n); pos += n;
        buf[pos++] = '\n';
    }
    buf[pos] = '\0';

    vfs_node_t* node = vfs_open(APKG_DB_VFS_PATH, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
    if (node) {
        vfs_write(node, 0, (size_t)pos, buf);
        vfs_close(node);
    }
}

void apkg_load(void)
{
    vfs_node_t* node = vfs_open(APKG_DB_VFS_PATH, VFS_O_READ);
    if (!node) return;

    char buf[4096];
    ssize_t n = vfs_read(node, 0, sizeof(buf) - 1, buf);
    vfs_close(node);
    if (n <= 0) return;
    buf[n] = '\0';

    /* Parse CSV lines */
    char* line = buf;
    while (*line && g_db_count < APKG_DB_MAX) {
        /* Split by comma */
        char* name_s = line;
        char* ver_s  = NULL;
        char* auth_s = NULL;

        char* p = line;
        int   field = 0;
        while (*p && *p != '\n') {
            if (*p == ',') {
                *p = '\0';
                field++;
                if (field == 1) ver_s  = p + 1;
                if (field == 2) auth_s = p + 1;
            }
            p++;
        }
        char* next = (*p == '\n') ? p + 1 : p;
        *p = '\0';

        if (name_s[0] && ver_s) {
            apkg_record_t* r = &g_db[g_db_count++];
            memset(r, 0, sizeof(*r));
            strncpy(r->name,    name_s, APKG_NAME_LEN - 1);
            strncpy(r->version, ver_s,  APKG_VER_LEN  - 1);
            if (auth_s) strncpy(r->author, auth_s, APKG_AUTHOR_LEN - 1);
            r->installed = true;
        }

        line = next;
    }

    kinfo("APKG: loaded %d package records from DB", g_db_count);
}
