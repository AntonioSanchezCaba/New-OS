/*
 * include/kernel/apkg.h — AetherOS Package Format (.apkg)
 *
 * Binary package format for distributing and installing applications.
 *
 * File layout:
 *   apkg_header_t   (256 bytes, fixed)
 *   apkg_dep_t[]    (dep_count entries × 48 bytes)
 *   uint8_t payload[payload_size]
 *
 * The payload is a raw flat binary: for kernel-space apps it is the
 * compiled .o object linked at load time; for future user-space apps
 * it will be an ELF binary.
 */
#pragma once
#include <types.h>

#define APKG_MAGIC       "APKG"
#define APKG_MAGIC_LEN   4
#define APKG_FMT_VERSION 1

#define APKG_NAME_LEN    32
#define APKG_VER_LEN     16
#define APKG_DESC_LEN    128
#define APKG_AUTHOR_LEN  32
#define APKG_MAX_DEPS    8
#define APKG_DEP_LEN     48
#define APKG_PATH_LEN    256

#define APKG_DB_MAX      64    /* Max installed packages tracked   */
#define APKG_CATALOG_MAX 128   /* Max packages in repo catalog     */

/* ---- On-disk header (256 bytes) ---- */
typedef struct __attribute__((packed)) {
    char     magic[4];              /* "APKG"                        */
    uint16_t fmt_version;           /* Format version (= 1)          */
    uint16_t flags;                 /* Reserved, set to 0            */
    char     name[APKG_NAME_LEN];   /* Package name (null-terminated)*/
    char     version[APKG_VER_LEN]; /* Semver string, e.g. "1.0.0"  */
    char     description[APKG_DESC_LEN]; /* Short description        */
    char     author[APKG_AUTHOR_LEN];    /* Author string            */
    uint8_t  dep_count;             /* Number of dependencies (0-8)  */
    uint8_t  _pad[3];
    uint32_t payload_size;          /* Byte size of binary payload   */
    uint32_t payload_crc32;         /* CRC32 of payload bytes        */
    uint8_t  _reserved[48];        /* Pad to 256 bytes              */
} apkg_header_t;

/* ---- Dependency entry (48 bytes) ---- */
typedef struct __attribute__((packed)) {
    char     name[APKG_NAME_LEN];   /* Required package name         */
    char     min_ver[APKG_VER_LEN]; /* Minimum version string        */
} apkg_dep_t;

/* ---- In-memory installed package record ---- */
typedef struct {
    char     name[APKG_NAME_LEN];
    char     version[APKG_VER_LEN];
    char     description[APKG_DESC_LEN];
    char     author[APKG_AUTHOR_LEN];
    bool     installed;
} apkg_record_t;

/*
 * ---- Package catalog entry (available-but-not-yet-installed) ----
 *
 * Populated by apkg_repo_scan(); represents a package found in a local
 * repository directory (e.g. /sys/packages/cache/).
 */
typedef struct {
    char     name[APKG_NAME_LEN];
    char     version[APKG_VER_LEN];
    char     description[APKG_DESC_LEN];
    char     author[APKG_AUTHOR_LEN];
    char     path[APKG_PATH_LEN];   /* VFS path to the .apkg file */
    bool     available;
} apkg_catalog_t;

/* ---- Installed package API ---- */

/* Initialize package subsystem (loads DB + scans default repo) */
void apkg_init(void);

/* Validate and install a package from a byte buffer.
 * Returns 0 on success, negative on error:
 *   -1 = bad magic/version/truncated
 *   -2 = CRC32 mismatch
 *   -3 = dependency not satisfied
 *   -4 = DB full
 *   -5 = already installed */
int  apkg_install(const uint8_t* pkg_data, size_t pkg_size);

/* Remove an installed package by name. Returns 0 on success. */
int  apkg_remove(const char* name);

/* Find installed package. Returns pointer or NULL. */
const apkg_record_t* apkg_find(const char* name);

/* Number of installed packages */
int  apkg_count(void);

/* Iterate installed packages (idx 0 .. apkg_count()-1) */
const apkg_record_t* apkg_get(int idx);

/* Save/load package DB to /sys/pkg/db via VFS */
void apkg_save(void);
void apkg_load(void);

/* ---- Repository / catalog API ---- */

/*
 * apkg_repo_scan - scan a VFS directory for .apkg files.
 * Reads each file's header to extract metadata and appends entries to
 * the in-memory catalog.  Safe to call multiple times with different dirs.
 * Returns the number of new catalog entries added.
 *
 * Default directory scanned at apkg_init() time: /sys/packages/cache
 */
int  apkg_repo_scan(const char* dir_path);

/*
 * apkg_repo_install_by_name - load an .apkg from the catalog and install it.
 * Reads the file into a heap buffer, calls apkg_install(), frees the buffer.
 * Returns 0 on success, negative on error (same codes as apkg_install()).
 */
int  apkg_repo_install_by_name(const char* name);

/* Number of packages in the catalog */
int  apkg_catalog_count(void);

/* Iterate catalog entries (idx 0 .. apkg_catalog_count()-1) */
const apkg_catalog_t* apkg_catalog_get(int idx);

/* CRC32 helper */
uint32_t apkg_crc32(const void* data, size_t len);
