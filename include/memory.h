/*
 * memory.h - Memory management interfaces
 *
 * Exposes the physical memory manager (PMM), virtual memory manager (VMM),
 * page table management, and kernel heap allocator.
 */
#ifndef MEMORY_H
#define MEMORY_H

#include <types.h>
#include <multiboot2.h>

/* ========== Physical Memory Manager ========== */

/* PMM constants */
#define PMM_FRAME_SIZE      PAGE_SIZE
#define PMM_FRAMES_PER_BYTE 8
#define PMM_MAX_MEMORY      (0x100000000ULL) /* 4GB physical */

/* PMM API */
void   pmm_init(struct multiboot2_info* mb2_info);
void*  pmm_alloc_frame(void);
void*  pmm_alloc_frames(size_t count);
void   pmm_free_frame(void* frame);
void   pmm_free_frames(void* frame, size_t count);
size_t pmm_free_frames_count(void);
size_t pmm_total_frames(void);
void   pmm_mark_used(uint64_t addr, size_t size);
void   pmm_mark_free(uint64_t addr, size_t size);

/* PMM reference counting (for COW) */
void    pmm_ref_frame(uint64_t phys);
void    pmm_unref_frame(uint64_t phys);
uint8_t pmm_get_refcount(uint64_t phys);

/* ========== Paging / VMM ========== */

/* Page table entry flags */
#define PTE_PRESENT     (1ULL << 0)
#define PTE_WRITABLE    (1ULL << 1)
#define PTE_USER        (1ULL << 2)
#define PTE_WRITE_THROUGH (1ULL << 3)
#define PTE_CACHE_DISABLE (1ULL << 4)
#define PTE_ACCESSED    (1ULL << 5)
#define PTE_DIRTY       (1ULL << 6)
#define PTE_HUGE        (1ULL << 7)
#define PTE_GLOBAL      (1ULL << 8)
/* Available bits 9-11 used for OS software flags */
#define PTE_COW         (1ULL << 9)   /* Copy-on-write shared page */
#define PTE_GUARD       (1ULL << 10)  /* Guard page (stack overflow sentinel) */
#define PTE_NX          (1ULL << 63)

/* Address mask for page table entries */
#define PTE_ADDR_MASK   0x000FFFFFFFFFF000ULL
#define PTE_FLAGS_MASK  (~PTE_ADDR_MASK)

/* Virtual address space regions */
#define USER_SPACE_START    0x0000000000400000ULL
#define USER_SPACE_END      0x00007FFFFFFFFFFFULL
#define USER_STACK_TOP      0x00007FFFFFFFE000ULL
#define USER_STACK_SIZE     (8 * 1024 * 1024) /* 8MB user stack */
/* Guard page sits one page below the stack mapping */
#define USER_STACK_GUARD    (USER_STACK_TOP - USER_STACK_SIZE - PAGE_SIZE)

#define KERNEL_HEAP_START   0xFFFFFFFF90000000ULL
#define KERNEL_HEAP_END     0xFFFFFFFFA0000000ULL

/* Framebuffer MMIO virtual window (PML4 index 510, avoids KERNEL_VMA_BASE overflow
 * for physical addresses >= 2GB such as the QEMU Bochs VBE framebuffer at 0xFD000000) */
#define FB_VIRT_BASE        0xFFFFFF0000000000ULL

/* User pointer validation: check ptr is within user space */
#define IS_USER_PTR(p)  ((uint64_t)(p) >= USER_SPACE_START && \
                         (uint64_t)(p) <  USER_SPACE_END)
#define VALIDATE_USER_PTR(p, len) \
    ((uint64_t)(p) >= USER_SPACE_START && \
     (uint64_t)(p) + (uint64_t)(len) <= USER_SPACE_END && \
     (uint64_t)(p) + (uint64_t)(len) >= (uint64_t)(p))

/* Page table level indices from virtual address */
#define PML4_INDEX(vaddr)   (((vaddr) >> 39) & 0x1FF)
#define PDPT_INDEX(vaddr)   (((vaddr) >> 30) & 0x1FF)
#define PD_INDEX(vaddr)     (((vaddr) >> 21) & 0x1FF)
#define PT_INDEX(vaddr)     (((vaddr) >> 12) & 0x1FF)

typedef uint64_t pml4_t;
typedef uint64_t pdpt_t;
typedef uint64_t pd_t;
typedef uint64_t pt_t;

/* Page directory/table type */
typedef struct {
    uint64_t entries[512];
} ALIGN(PAGE_SIZE) page_table_t;

/* VMM API */
void   vmm_init(void);
page_table_t* vmm_create_address_space(void);
void   vmm_destroy_address_space(page_table_t* pml4);
void   vmm_switch_address_space(page_table_t* pml4);
page_table_t* vmm_current_address_space(void);
int    vmm_map_page(page_table_t* pml4, uint64_t vaddr, uint64_t paddr, uint64_t flags);
int    vmm_unmap_page(page_table_t* pml4, uint64_t vaddr);
uint64_t vmm_get_physical(page_table_t* pml4, uint64_t vaddr);
int    vmm_map_range(page_table_t* pml4, uint64_t vaddr, uint64_t paddr, size_t size, uint64_t flags);

/* COW-aware clone for fork() */
page_table_t* vmm_clone_address_space(page_table_t* src);
/* Map a guard page (triggers fault on access) */
int    vmm_map_guard_page(page_table_t* pml4, uint64_t vaddr);
void   vmm_handle_page_fault(uint64_t fault_addr, uint64_t error_code);

/* Kernel page table (used at boot) */
extern page_table_t* kernel_pml4;

/* ========== Kernel Heap ========== */

/* Heap API */
void   heap_init(uint64_t start, size_t size);
void*  kmalloc(size_t size);
void*  kmalloc_aligned(size_t size, size_t align);
void*  kcalloc(size_t count, size_t size);
void*  krealloc(void* ptr, size_t new_size);
void   kfree(void* ptr);
size_t kheap_used(void);
size_t kheap_free(void);

#endif /* MEMORY_H */
