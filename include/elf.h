/*
 * elf.h - ELF (Executable and Linkable Format) structures for x86_64
 *
 * Used by the kernel ELF loader to load user-space executables.
 */
#ifndef ELF_H
#define ELF_H

#include <types.h>

/* ELF magic number */
#define ELF_MAGIC   0x464C457F  /* "\x7fELF" in little-endian */

/* ELF class */
#define ELFCLASS32  1
#define ELFCLASS64  2

/* ELF data encoding */
#define ELFDATA2LSB 1  /* Little endian */
#define ELFDATA2MSB 2  /* Big endian */

/* ELF file type */
#define ET_NONE   0
#define ET_REL    1  /* Relocatable */
#define ET_EXEC   2  /* Executable */
#define ET_DYN    3  /* Shared object */
#define ET_CORE   4

/* ELF machine type */
#define EM_X86_64 62

/* ELF version */
#define EV_CURRENT 1

/* Program header segment types */
#define PT_NULL     0
#define PT_LOAD     1
#define PT_DYNAMIC  2
#define PT_INTERP   3
#define PT_NOTE     4
#define PT_SHLIB    5
#define PT_PHDR     6

/* Program header flags */
#define PF_X  0x1  /* Execute */
#define PF_W  0x2  /* Write */
#define PF_R  0x4  /* Read */

/* Section header types */
#define SHT_NULL     0
#define SHT_PROGBITS 1
#define SHT_SYMTAB   2
#define SHT_STRTAB   3
#define SHT_RELA     4
#define SHT_HASH     5
#define SHT_DYNAMIC  6
#define SHT_NOTE     7
#define SHT_NOBITS   8
#define SHT_REL      9

/* ELF64 header */
typedef struct {
    uint8_t  e_ident[16];   /* Magic, class, data, version, OS/ABI */
    uint16_t e_type;         /* Object file type */
    uint16_t e_machine;      /* Architecture */
    uint32_t e_version;      /* ELF version */
    uint64_t e_entry;        /* Entry point virtual address */
    uint64_t e_phoff;        /* Program header table offset */
    uint64_t e_shoff;        /* Section header table offset */
    uint32_t e_flags;        /* Processor-specific flags */
    uint16_t e_ehsize;       /* ELF header size */
    uint16_t e_phentsize;    /* Program header entry size */
    uint16_t e_phnum;        /* Number of program headers */
    uint16_t e_shentsize;    /* Section header entry size */
    uint16_t e_shnum;        /* Number of section headers */
    uint16_t e_shstrndx;    /* Section name string table index */
} PACKED elf64_hdr_t;

/* ELF64 program header */
typedef struct {
    uint32_t p_type;    /* Segment type */
    uint32_t p_flags;   /* Segment flags */
    uint64_t p_offset;  /* Offset in file */
    uint64_t p_vaddr;   /* Virtual address in memory */
    uint64_t p_paddr;   /* Physical address */
    uint64_t p_filesz;  /* Size of segment in file */
    uint64_t p_memsz;   /* Size of segment in memory */
    uint64_t p_align;   /* Alignment */
} PACKED elf64_phdr_t;

/* ELF64 section header */
typedef struct {
    uint32_t sh_name;       /* Section name index in shstrtab */
    uint32_t sh_type;       /* Section type */
    uint64_t sh_flags;      /* Section flags */
    uint64_t sh_addr;       /* Virtual address */
    uint64_t sh_offset;     /* Offset in file */
    uint64_t sh_size;       /* Section size */
    uint32_t sh_link;       /* Link to another section */
    uint32_t sh_info;       /* Additional info */
    uint64_t sh_addralign;  /* Alignment */
    uint64_t sh_entsize;    /* Entry size if section has table */
} PACKED elf64_shdr_t;

/* ELF loader API */
int  elf_load(process_t* proc, const void* data, size_t size, uint64_t* entry);
bool elf_validate(const void* data, size_t size);

#endif /* ELF_H */
