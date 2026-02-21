/*
 * include/kernel/pkg.h - Aureon OS Package Manager (APM)
 *
 * A lightweight package management layer embedded in the kernel.
 * Packages are self-contained archives stored on the FAT32 or ramfs.
 *
 * Package format (.aur):
 *   [Header: magic, version, name, description, version string, deps]
 *   [File table: count + entries with path + offset + size + perms]
 *   [Payload: concatenated file data, zstd-compressed (future)]
 *
 * The kernel PKG layer handles:
 *   1. Loading the package database from /sys/packages/db
 *   2. Installing packages (extracting files to VFS)
 *   3. Tracking installed packages and their file lists
 *   4. Uninstalling (removing files from VFS)
 *   5. Dependency resolution (topological sort)
 *
 * In the current implementation packages are uncompressed flat archives
 * since we have no decompressor. Compression is stubbed for future zstd.
 */
#ifndef KERNEL_PKG_H
#define KERNEL_PKG_H

#include <types.h>

#define PKG_MAGIC          0x41555250u  /* "AURP" */
#define PKG_VERSION        1
#define PKG_NAME_MAX       64
#define PKG_DESC_MAX       256
#define PKG_VER_MAX        32
#define PKG_MAX_DEPS       16
#define PKG_MAX_FILES      256
#define PKG_MAX_INSTALLED  128
#define PKG_DB_PATH        "/sys/packages/db"
#define PKG_CACHE_PATH     "/sys/packages/cache"
#define PKG_INSTALL_ROOT   "/"

/* ── On-disk package header ──────────────────────────────────────────── */
typedef struct {
    uint32_t magic;                     /* PKG_MAGIC */
    uint32_t format_version;            /* PKG_VERSION */
    char     name[PKG_NAME_MAX];
    char     description[PKG_DESC_MAX];
    char     version[PKG_VER_MAX];
    char     author[PKG_NAME_MAX];
    uint32_t num_deps;
    char     deps[PKG_MAX_DEPS][PKG_NAME_MAX];
    uint32_t num_files;
    uint32_t payload_offset;            /* Byte offset of payload in .aur file */
    uint32_t payload_size;
    uint32_t checksum;                  /* CRC32 of payload */
    uint8_t  reserved[64];
} PACKED pkg_header_t;

/* ── File entry inside a package ─────────────────────────────────────── */
typedef struct {
    char     path[256];         /* Install path (e.g. "/bin/myapp") */
    uint32_t offset;            /* Offset within payload */
    uint32_t size;              /* Uncompressed size */
    uint16_t mode;              /* Unix-style permissions */
    uint8_t  type;              /* PKG_FILE_REG, PKG_FILE_DIR, PKG_FILE_LINK */
    uint8_t  reserved;
} PACKED pkg_file_entry_t;

#define PKG_FILE_REG  0   /* Regular file */
#define PKG_FILE_DIR  1   /* Directory */
#define PKG_FILE_LINK 2   /* Symbolic link */

/* ── Installed package record (in-memory database) ───────────────────── */
typedef struct {
    bool     valid;
    char     name[PKG_NAME_MAX];
    char     version[PKG_VER_MAX];
    char     description[PKG_DESC_MAX];
    uint32_t num_files;
    char     files[PKG_MAX_FILES][256];  /* Installed file paths */
    uint64_t install_time;               /* Timer ticks at install */
} pkg_entry_t;

/* ── Package database ────────────────────────────────────────────────── */
typedef struct {
    uint32_t    count;
    pkg_entry_t packages[PKG_MAX_INSTALLED];
} pkg_db_t;

extern pkg_db_t g_pkg_db;

/* ── Package manager API ─────────────────────────────────────────────── */
void pkg_init(void);                         /* Load DB from disk */
void pkg_db_save(void);                      /* Persist DB to disk */

/* Install from a .aur archive file on the VFS */
int  pkg_install(const char* aur_path);

/* Uninstall by package name */
int  pkg_uninstall(const char* name);

/* Query */
pkg_entry_t* pkg_find(const char* name);
bool         pkg_installed(const char* name);

/* List all installed packages (returns count) */
int  pkg_list(pkg_entry_t** out, int max);

/* Build a package archive (for development tools) */
int  pkg_build(const char* src_dir, const char* pkg_name,
                const char* version, const char* out_path);

/* Verify a package's checksum */
int  pkg_verify(const char* aur_path);

/* Simple CRC32 (used for package integrity) */
uint32_t pkg_crc32(const void* data, size_t len);

/* Syscall entry points */
int64_t sys_pkg_install(uint64_t path_addr, uint64_t a2, uint64_t a3,
                         uint64_t a4, uint64_t a5, uint64_t a6);
int64_t sys_pkg_remove(uint64_t name_addr, uint64_t a2, uint64_t a3,
                        uint64_t a4, uint64_t a5, uint64_t a6);
int64_t sys_pkg_list(uint64_t buf_addr, uint64_t max, uint64_t a3,
                      uint64_t a4, uint64_t a5, uint64_t a6);

#endif /* KERNEL_PKG_H */
