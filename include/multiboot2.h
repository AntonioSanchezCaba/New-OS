/*
 * multiboot2.h - Multiboot2 specification structures
 *
 * Defines the structures used to parse information passed by GRUB/Multiboot2
 * bootloaders to the kernel at startup.
 */
#ifndef MULTIBOOT2_H
#define MULTIBOOT2_H

#include <types.h>

/* Multiboot2 magic values */
#define MULTIBOOT2_MAGIC            0xE85250D6
#define MULTIBOOT2_BOOTLOADER_MAGIC 0x36D76289

/* Multiboot2 header flags */
#define MULTIBOOT2_ARCHITECTURE_I386  0
#define MULTIBOOT2_ARCHITECTURE_MIPS  4

/* Multiboot2 tag types */
#define MULTIBOOT2_TAG_END              0
#define MULTIBOOT2_TAG_CMDLINE          1
#define MULTIBOOT2_TAG_BOOT_LOADER_NAME 2
#define MULTIBOOT2_TAG_MODULE           3
#define MULTIBOOT2_TAG_BASIC_MEMINFO    4
#define MULTIBOOT2_TAG_BOOTDEV          5
#define MULTIBOOT2_TAG_MMAP             6
#define MULTIBOOT2_TAG_VBE              7
#define MULTIBOOT2_TAG_FRAMEBUFFER      8
#define MULTIBOOT2_TAG_ELF_SECTIONS     9
#define MULTIBOOT2_TAG_APM              10
#define MULTIBOOT2_TAG_EFI32            11
#define MULTIBOOT2_TAG_EFI64            12
#define MULTIBOOT2_TAG_SMBIOS           13
#define MULTIBOOT2_TAG_ACPI_OLD         14
#define MULTIBOOT2_TAG_ACPI_NEW         15
#define MULTIBOOT2_TAG_NETWORK          16
#define MULTIBOOT2_TAG_EFI_MMAP         17

/* Memory map entry types */
#define MULTIBOOT2_MMAP_AVAILABLE        1
#define MULTIBOOT2_MMAP_RESERVED         2
#define MULTIBOOT2_MMAP_ACPI_RECLAIMABLE 3
#define MULTIBOOT2_MMAP_NVS              4
#define MULTIBOOT2_MMAP_BAD              5

/* Multiboot2 boot information header */
struct multiboot2_info {
    uint32_t total_size;
    uint32_t reserved;
} PACKED;

/* Generic tag structure */
struct multiboot2_tag {
    uint32_t type;
    uint32_t size;
} PACKED;

/* Command line tag */
struct multiboot2_tag_string {
    uint32_t type;
    uint32_t size;
    char string[0];
} PACKED;

/* Basic memory info tag */
struct multiboot2_tag_basic_meminfo {
    uint32_t type;
    uint32_t size;
    uint32_t mem_lower;  /* KB below 1MB */
    uint32_t mem_upper;  /* KB above 1MB */
} PACKED;

/* Memory map entry */
struct multiboot2_mmap_entry {
    uint64_t base_addr;
    uint64_t length;
    uint32_t type;
    uint32_t zero;
} PACKED;

/* Memory map tag */
struct multiboot2_tag_mmap {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
    struct multiboot2_mmap_entry entries[0];
} PACKED;

/* Module tag */
struct multiboot2_tag_module {
    uint32_t type;
    uint32_t size;
    uint32_t mod_start;
    uint32_t mod_end;
    char cmdline[0];
} PACKED;

/* Framebuffer tag */
struct multiboot2_tag_framebuffer {
    uint32_t type;
    uint32_t size;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t  framebuffer_bpp;
    uint8_t  framebuffer_type;
    uint16_t reserved;
} PACKED;

/* Helper macro to iterate over Multiboot2 tags */
#define MULTIBOOT2_TAG_NEXT(tag) \
    ((struct multiboot2_tag*)((uint8_t*)(tag) + ALIGN_UP((tag)->size, 8)))

/* Functions to parse multiboot2 info */
struct multiboot2_tag* multiboot2_find_tag(struct multiboot2_info* info, uint32_t type);

#endif /* MULTIBOOT2_H */
