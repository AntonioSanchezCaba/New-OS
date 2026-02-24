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

#define APKG_DB_VFS_PATH  "/sys/pkg/db"

void apkg_init(void)
{
    memset(g_db, 0, sizeof(g_db));
    g_db_count   = 0;
    g_initialized = true;
    apkg_load();
    kinfo("APKG: initialized (%d packages installed)", g_db_count);
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
