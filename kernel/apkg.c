/*
 * kernel/apkg.c — AetherOS Package Format (.apkg) implementation
 *
 * Manages installed package records in an in-memory database (max 64 entries).
 * Validates package headers, checks dependencies, verifies CRC32, records
 * installs, and executes ELF payloads via apkg_exec().
 *
 * Execution model:
 *   apkg_exec() reads the .apkg payload, validates it as a statically-linked
 *   ELF64 binary, creates a user-space process, maps all PT_LOAD segments,
 *   allocates a user stack, and hands it to the scheduler.  Built-in ARE
 *   surfaces are still linked directly into the kernel; apkg_exec() is for
 *   packages distributed as .apkg files containing ELF payloads.
 */
#include <kernel/apkg.h>
#include <fs/vfs.h>
#include <memory.h>
#include <string.h>
#include <kernel.h>
#include <elf.h>
#include <process.h>
#include <scheduler.h>

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
    vfs_dirent_t de;

    while (vfs_readdir(dir, idx++, &de) == 0) {
        /* Only process names ending in ".apkg" */
        size_t nlen = strlen(de.name);
        if (nlen < 6 ||
            strcmp(de.name + nlen - 5, ".apkg") != 0)
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
        memcpy(fpath + dlen + 1, de.name, nlen + 1);

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

/* =========================================================
 * ELF payload execution
 * ========================================================= */

/*
 * User-space stack layout for launched processes.
 * We reserve USER_STACK_PAGES pages just below USER_STACK_TOP.
 */
#define APKG_USER_STACK_TOP   0x7FFFFFFFF000ULL
#define APKG_USER_STACK_PAGES 8

/*
 * apkg_exec — launch the ELF payload of an installed package.
 *
 * Steps:
 *   1. Find the installed package record to get its name.
 *   2. Locate the .apkg file path from the catalog.
 *   3. Read the file into a heap buffer.
 *   4. Locate the payload region (past header + dep table).
 *   5. Validate the payload as ELF64 x86_64 exec.
 *   6. Create a new user-space process.
 *   7. Load all PT_LOAD segments via elf_load().
 *   8. Allocate and map a user stack.
 *   9. Set up the initial CPU context.
 *  10. Enqueue with the scheduler.
 *
 * Returns the new PID on success, -1 on error.
 */
pid_t apkg_exec(const char* name)
{
    if (!name) return -1;

    /* 1. Verify it's installed */
    const apkg_record_t* rec = apkg_find(name);
    if (!rec) {
        klog_warn("APKG exec: '%s' is not installed", name);
        return -1;
    }

    /* 2. Find .apkg file path in catalog */
    const apkg_catalog_t* ce = NULL;
    for (int i = 0; i < g_catalog_count; i++) {
        if (strncmp(g_catalog[i].name, name, APKG_NAME_LEN) == 0) {
            ce = &g_catalog[i];
            break;
        }
    }
    if (!ce) {
        klog_warn("APKG exec: '%s' not found in catalog (no .apkg path)", name);
        return -1;
    }

    /* 3. Open and read the entire .apkg file */
    vfs_node_t* fnode = vfs_open(ce->path, VFS_O_READ);
    if (!fnode) {
        klog_warn("APKG exec: cannot open '%s'", ce->path);
        return -1;
    }

    /* Determine file size */
    size_t file_size = 0;
    {
        char probe[512];
        ssize_t r;
        while ((r = vfs_read(fnode, (off_t)file_size, sizeof(probe), probe)) > 0)
            file_size += (size_t)r;
    }
    if (file_size < sizeof(apkg_header_t)) {
        vfs_close(fnode);
        return -1;
    }

    uint8_t* buf = (uint8_t*)kmalloc(file_size);
    if (!buf) { vfs_close(fnode); return -1; }

    ssize_t nr = vfs_read(fnode, 0, file_size, buf);
    vfs_close(fnode);
    if (nr < (ssize_t)file_size) { kfree(buf); return -1; }

    /* 4. Locate payload region */
    const apkg_header_t* hdr = (const apkg_header_t*)buf;
    if (memcmp(hdr->magic, APKG_MAGIC, APKG_MAGIC_LEN) != 0) {
        kfree(buf);
        return -1;
    }

    size_t payload_off  = sizeof(apkg_header_t)
                        + (size_t)hdr->dep_count * sizeof(apkg_dep_t);
    size_t payload_size = hdr->payload_size;

    if (payload_size == 0 || payload_off + payload_size > file_size) {
        klog_warn("APKG exec: '%s' has no executable payload", name);
        kfree(buf);
        return -1;
    }

    const uint8_t* payload = buf + payload_off;

    /* 5. Validate as ELF64 x86_64 executable */
    if (!elf_validate(payload, payload_size)) {
        klog_warn("APKG exec: '%s' payload is not a valid ELF64 binary", name);
        kfree(buf);
        return -1;
    }

    /* 6. Create user-space process (entry=NULL; we set context after ELF load) */
    process_t* proc = process_create(rec->name, NULL, false);
    if (!proc) {
        klog_warn("APKG exec: failed to create process for '%s'", name);
        kfree(buf);
        return -1;
    }

    /* 7. Load ELF segments into the process address space */
    uint64_t entry_vaddr = 0;
    if (elf_load(proc, payload, payload_size, &entry_vaddr) != 0) {
        klog_warn("APKG exec: elf_load failed for '%s'", name);
        kfree(buf);
        /* process_exit cleans up address space */
        process_exit(proc, -1);
        return -1;
    }
    kfree(buf);

    /* 8. Allocate and map user stack */
    for (int i = 0; i < APKG_USER_STACK_PAGES; i++) {
        void* phys = pmm_alloc_frame();
        if (!phys) {
            klog_warn("APKG exec: out of memory for stack");
            process_exit(proc, -1);
            return -1;
        }
        memset(PHYS_TO_VIRT(phys), 0, PAGE_SIZE);
        uint64_t vaddr = APKG_USER_STACK_TOP - (uint64_t)(i + 1) * PAGE_SIZE;
        vmm_map_page(proc->address_space, vaddr, (uint64_t)phys,
                     PTE_PRESENT | PTE_WRITABLE | PTE_USER | PTE_NX);
    }
    proc->user_stack = APKG_USER_STACK_TOP;

    /* 9. Set up initial CPU context for ring-3 entry */
    proc->context.rip    = entry_vaddr;
    proc->context.rsp    = APKG_USER_STACK_TOP - 8;  /* 16-byte aligned - 8 */
    proc->context.rflags = 0x202;   /* IF=1, reserved bit 1 */
    proc->context.cs     = 0x23;    /* user code segment (ring 3) */
    proc->context.ss     = 0x1B;    /* user data segment (ring 3) */
    proc->state          = PROC_STATE_READY;

    /* 10. Hand to scheduler */
    scheduler_add(proc);

    kinfo("APKG exec: launched '%s' (pid=%d, entry=0x%llx)",
          name, proc->pid, (unsigned long long)entry_vaddr);
    return proc->pid;
}
