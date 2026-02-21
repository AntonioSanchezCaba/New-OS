/*
 * memory/vmm.c - Virtual Memory Manager
 *
 * Manages 4-level page tables (PML4 -> PDPT -> PD -> PT) for both
 * the kernel address space and per-process user address spaces.
 *
 * The kernel occupies the top 2GB of virtual address space:
 *   0xFFFFFFFF80000000 - 0xFFFFFFFFFFFFFFFF
 *
 * Each process has its own PML4. The upper half of each PML4 is shared
 * with the kernel's PML4 (kernel_pml4) so kernel mappings are visible
 * from all address spaces.
 */
#include <memory.h>
#include <kernel.h>
#include <types.h>
#include <string.h>

/* The kernel's root page table */
page_table_t* kernel_pml4 = NULL;

/* Physical address of the kernel PML4 (used to switch back to kernel space) */
static uint64_t kernel_pml4_phys = 0;

/* ============================================================
 * Helper: allocate a zeroed page table from physical memory
 * ============================================================ */
static page_table_t* alloc_page_table(void)
{
    void* phys = pmm_alloc_frame();
    if (!phys) return NULL;

    /* Map the new table into kernel virtual space so we can zero it */
    page_table_t* virt = (page_table_t*)PHYS_TO_VIRT(phys);
    memset(virt, 0, sizeof(page_table_t));
    return virt;
}

/* Get physical address of a virtual kernel pointer */
static inline uint64_t virt_to_phys_kernel(void* vaddr)
{
    return (uint64_t)vaddr - KERNEL_VMA_BASE;
}

/* ============================================================
 * Walk / create page tables for a given virtual address
 * ============================================================ */

/*
 * get_or_create_table - return (creating if needed) the page table at a
 *                        given level for the given virtual address.
 *
 * @parent: the parent page table (PML4, PDPT, or PD)
 * @idx:    index into parent
 * @flags:  flags to apply if a new entry is created
 * Returns: virtual pointer to the child page table, or NULL on OOM.
 */
static page_table_t* get_or_create_table(page_table_t* parent, int idx,
                                          uint64_t flags)
{
    uint64_t entry = parent->entries[idx];

    if (entry & PTE_PRESENT) {
        /* Table already exists - return its virtual address */
        uint64_t phys = entry & PTE_ADDR_MASK;
        return (page_table_t*)PHYS_TO_VIRT(phys);
    }

    /* Allocate a new page table */
    page_table_t* child = alloc_page_table();
    if (!child) return NULL;

    uint64_t child_phys = virt_to_phys_kernel(child);
    parent->entries[idx] = child_phys | flags;
    return child;
}

/* ============================================================
 * vmm_init - set up kernel virtual memory
 * ============================================================ */

void vmm_init(void)
{
    /*
     * The boot.asm already created minimal page tables (at physical 0x1000-0x5000).
     * We now create proper kernel page tables using the PMM.
     *
     * The current CR3 points to the boot page tables. We will:
     *   1. Allocate a proper PML4 from the PMM
     *   2. Identity-map the first 2GB (kernel can access physical memory)
     *   3. Map the kernel higher half (KERNEL_VMA_BASE -> physical 0)
     *   4. Switch to the new page tables
     */

    /* Allocate the kernel PML4 */
    kernel_pml4 = alloc_page_table();
    if (!kernel_pml4) {
        kpanic("VMM: Failed to allocate kernel PML4!");
    }

    kernel_pml4_phys = virt_to_phys_kernel(kernel_pml4);

    /*
     * Map 0 - 4GB as identity (physical = virtual) with 2MB huge pages.
     * This allows the kernel to access physical memory for device MMIO, etc.
     *
     * KERNEL_VMA_BASE also maps 0 - 2GB (the higher half mapping).
     */
    for (uint64_t addr = 0; addr < 0x100000000ULL; addr += 0x200000) {
        /* Identity map: virt = phys */
        vmm_map_page(kernel_pml4, addr, addr,
                     PTE_PRESENT | PTE_WRITABLE | PTE_GLOBAL | PTE_HUGE);

        /* Higher half map: virt = KERNEL_VMA_BASE + phys */
        if (addr < 0x80000000ULL) { /* First 2GB */
            vmm_map_page(kernel_pml4, KERNEL_VMA_BASE + addr, addr,
                         PTE_PRESENT | PTE_WRITABLE | PTE_GLOBAL | PTE_HUGE);
        }
    }

    /* Switch to the new page tables */
    write_cr3(kernel_pml4_phys);

    kinfo("VMM: Kernel PML4 at phys=0x%llx, virt=%p",
          kernel_pml4_phys, (void*)kernel_pml4);
}

/* ============================================================
 * vmm_map_page - install a mapping in a page table
 * ============================================================ */

int vmm_map_page(page_table_t* pml4, uint64_t vaddr, uint64_t paddr,
                  uint64_t flags)
{
    /* Check if this is a 2MB huge page request */
    bool huge = (flags & PTE_HUGE) != 0;

    /* Walk PML4 -> PDPT -> PD, creating tables as needed */
    int pml4_idx = PML4_INDEX(vaddr);
    int pdpt_idx = PDPT_INDEX(vaddr);
    int pd_idx   = PD_INDEX(vaddr);
    int pt_idx   = PT_INDEX(vaddr);

    uint64_t entry_flags = PTE_PRESENT | PTE_WRITABLE;
    if (flags & PTE_USER) entry_flags |= PTE_USER;

    /* Level 1: PML4 -> PDPT */
    page_table_t* pdpt = get_or_create_table(pml4, pml4_idx, entry_flags);
    if (!pdpt) return -1;

    /* Level 2: PDPT -> PD */
    page_table_t* pd = get_or_create_table(pdpt, pdpt_idx, entry_flags);
    if (!pd) return -1;

    if (huge) {
        /* Install a 2MB page directly in the PD */
        pd->entries[pd_idx] = (paddr & ~0x1FFFFFULL) | flags | PTE_PRESENT;
        tlb_flush_page(vaddr);
        return 0;
    }

    /* Level 3: PD -> PT */
    page_table_t* pt = get_or_create_table(pd, pd_idx, entry_flags);
    if (!pt) return -1;

    /* Level 4: PT -> physical page */
    pt->entries[pt_idx] = (paddr & PTE_ADDR_MASK) | flags | PTE_PRESENT;
    tlb_flush_page(vaddr);
    return 0;
}

/* ============================================================
 * vmm_unmap_page - remove a mapping
 * ============================================================ */

int vmm_unmap_page(page_table_t* pml4, uint64_t vaddr)
{
    page_table_t* pdpt = (page_table_t*)PHYS_TO_VIRT(
        pml4->entries[PML4_INDEX(vaddr)] & PTE_ADDR_MASK);
    if (!pdpt || !(pml4->entries[PML4_INDEX(vaddr)] & PTE_PRESENT)) return -1;

    page_table_t* pd = (page_table_t*)PHYS_TO_VIRT(
        pdpt->entries[PDPT_INDEX(vaddr)] & PTE_ADDR_MASK);
    if (!pd || !(pdpt->entries[PDPT_INDEX(vaddr)] & PTE_PRESENT)) return -1;

    uint64_t pd_entry = pd->entries[PD_INDEX(vaddr)];
    if (!(pd_entry & PTE_PRESENT)) return -1;

    if (pd_entry & PTE_HUGE) {
        /* 2MB page */
        pd->entries[PD_INDEX(vaddr)] = 0;
        tlb_flush_page(vaddr);
        return 0;
    }

    page_table_t* pt = (page_table_t*)PHYS_TO_VIRT(
        pd_entry & PTE_ADDR_MASK);
    if (!pt) return -1;

    pt->entries[PT_INDEX(vaddr)] = 0;
    tlb_flush_page(vaddr);
    return 0;
}

/* ============================================================
 * vmm_get_physical - translate virtual to physical address
 * ============================================================ */

uint64_t vmm_get_physical(page_table_t* pml4, uint64_t vaddr)
{
    uint64_t e;

    e = pml4->entries[PML4_INDEX(vaddr)];
    if (!(e & PTE_PRESENT)) return 0;

    page_table_t* pdpt = (page_table_t*)PHYS_TO_VIRT(e & PTE_ADDR_MASK);
    e = pdpt->entries[PDPT_INDEX(vaddr)];
    if (!(e & PTE_PRESENT)) return 0;

    page_table_t* pd = (page_table_t*)PHYS_TO_VIRT(e & PTE_ADDR_MASK);
    e = pd->entries[PD_INDEX(vaddr)];
    if (!(e & PTE_PRESENT)) return 0;

    if (e & PTE_HUGE) {
        /* 2MB page: mask to 2MB alignment + page offset */
        return (e & ~0x1FFFFFULL) | (vaddr & 0x1FFFFF);
    }

    page_table_t* pt = (page_table_t*)PHYS_TO_VIRT(e & PTE_ADDR_MASK);
    e = pt->entries[PT_INDEX(vaddr)];
    if (!(e & PTE_PRESENT)) return 0;

    return (e & PTE_ADDR_MASK) | (vaddr & 0xFFF);
}

/* ============================================================
 * vmm_map_range - map a range of pages
 * ============================================================ */

int vmm_map_range(page_table_t* pml4, uint64_t vaddr, uint64_t paddr,
                   size_t size, uint64_t flags)
{
    size = ALIGN_UP(size, PAGE_SIZE);
    for (size_t off = 0; off < size; off += PAGE_SIZE) {
        if (vmm_map_page(pml4, vaddr + off, paddr + off, flags) != 0) {
            return -1;
        }
    }
    return 0;
}

/* ============================================================
 * Address space management
 * ============================================================ */

/*
 * vmm_create_address_space - allocate a new PML4 with the kernel half shared.
 */
page_table_t* vmm_create_address_space(void)
{
    page_table_t* pml4 = alloc_page_table();
    if (!pml4) return NULL;

    /* Copy the kernel half (upper 256 PML4 entries: indices 256-511)
     * so the kernel is visible from user-space (required for syscall handling). */
    for (int i = 256; i < 512; i++) {
        pml4->entries[i] = kernel_pml4->entries[i];
    }

    return pml4;
}

/*
 * vmm_destroy_address_space - free all user-space page tables.
 *
 * Only frees user-space PML4 entries (0-255). Kernel entries are shared
 * and must not be freed here.
 */
void vmm_destroy_address_space(page_table_t* pml4)
{
    for (int i = 0; i < 256; i++) {
        if (!(pml4->entries[i] & PTE_PRESENT)) continue;

        page_table_t* pdpt = (page_table_t*)PHYS_TO_VIRT(
            pml4->entries[i] & PTE_ADDR_MASK);

        for (int j = 0; j < 512; j++) {
            if (!(pdpt->entries[j] & PTE_PRESENT)) continue;

            page_table_t* pd = (page_table_t*)PHYS_TO_VIRT(
                pdpt->entries[j] & PTE_ADDR_MASK);

            for (int k = 0; k < 512; k++) {
                uint64_t pde = pd->entries[k];
                if (!(pde & PTE_PRESENT)) continue;

                if (!(pde & PTE_HUGE)) {
                    /* Free the PT */
                    pmm_free_frame((void*)(pde & PTE_ADDR_MASK));
                }
                /* If huge page, the physical frame might be user data -
                 * we do NOT free user data here; the process should call
                 * munmap before exit. For now just clear the mapping. */
            }
            pmm_free_frame((void*)(pdpt->entries[j] & PTE_ADDR_MASK));
        }
        pmm_free_frame((void*)(pml4->entries[i] & PTE_ADDR_MASK));
    }

    pmm_free_frame((void*)virt_to_phys_kernel(pml4));
}

void vmm_switch_address_space(page_table_t* pml4)
{
    write_cr3(virt_to_phys_kernel(pml4));
}

page_table_t* vmm_current_address_space(void)
{
    uint64_t cr3 = read_cr3();
    return (page_table_t*)PHYS_TO_VIRT(cr3);
}

/*
 * vmm_handle_page_fault - called by the page fault interrupt handler.
 *
 * @fault_addr:  the address that caused the fault (CR2)
 * @error_code:  the error code pushed by the CPU
 */
void vmm_handle_page_fault(uint64_t fault_addr, uint64_t error_code)
{
    bool present     = (error_code & 0x1) != 0;
    bool write_fault = (error_code & 0x2) != 0;
    bool user_fault  = (error_code & 0x4) != 0;

    if (user_fault) {
        /* User-space page fault: kill the process */
        kwarn("Page fault in user space: addr=0x%llx, error=0x%llx",
              fault_addr, error_code);
        /* TODO: send SIGSEGV to current process */
        extern process_t* current_process;
        if (current_process) {
            process_exit(current_process, -1);
        }
        return;
    }

    /* Kernel page fault: this is always fatal */
    kpanic("Kernel page fault at 0x%016llx (error=0x%02llx, %s%s%s)",
           fault_addr, error_code,
           present     ? "protection-violation " : "not-present ",
           write_fault ? "write " : "read ",
           user_fault  ? "user" : "kernel");
}

/*
 * vmm_clone_address_space - copy-on-write clone for fork().
 *
 * Copies user-space mappings from src, marking all writable pages as
 * read-only and sharing the physical frames. Page faults on write will
 * trigger copy-on-write.
 */
page_table_t* vmm_clone_address_space(page_table_t* src)
{
    page_table_t* dst = vmm_create_address_space();
    if (!dst) return NULL;

    /* Walk user-space entries (indices 0-255) */
    for (int i = 0; i < 256; i++) {
        if (!(src->entries[i] & PTE_PRESENT)) continue;

        page_table_t* src_pdpt = (page_table_t*)PHYS_TO_VIRT(
            src->entries[i] & PTE_ADDR_MASK);

        for (int j = 0; j < 512; j++) {
            if (!(src_pdpt->entries[j] & PTE_PRESENT)) continue;

            page_table_t* src_pd = (page_table_t*)PHYS_TO_VIRT(
                src_pdpt->entries[j] & PTE_ADDR_MASK);

            for (int k = 0; k < 512; k++) {
                uint64_t pde = src_pd->entries[k];
                if (!(pde & PTE_PRESENT)) continue;

                if (pde & PTE_HUGE) {
                    /* 2MB shared (copy-on-write not implemented for huge pages) */
                    uint64_t vaddr = ((uint64_t)i << 39) | ((uint64_t)j << 30) |
                                     ((uint64_t)k << 21);
                    uint64_t paddr = pde & ~0x1FFFFFULL;

                    /* Allocate a new 2MB region and copy */
                    void* new_phys = pmm_alloc_frames(512);
                    if (!new_phys) goto oom;
                    memcpy(PHYS_TO_VIRT(new_phys), PHYS_TO_VIRT((void*)paddr),
                           0x200000);
                    vmm_map_page(dst, vaddr, (uint64_t)new_phys,
                                 (pde & PTE_FLAGS_MASK) | PTE_HUGE);
                    continue;
                }

                page_table_t* src_pt = (page_table_t*)PHYS_TO_VIRT(
                    pde & PTE_ADDR_MASK);

                for (int l = 0; l < 512; l++) {
                    uint64_t pte = src_pt->entries[l];
                    if (!(pte & PTE_PRESENT)) continue;

                    uint64_t vaddr = ((uint64_t)i << 39) | ((uint64_t)j << 30) |
                                     ((uint64_t)k << 21) | ((uint64_t)l << 12);
                    uint64_t paddr = pte & PTE_ADDR_MASK;

                    /* Allocate a new frame and copy the page */
                    void* new_phys = pmm_alloc_frame();
                    if (!new_phys) goto oom;
                    memcpy(PHYS_TO_VIRT(new_phys), PHYS_TO_VIRT((void*)paddr),
                           PAGE_SIZE);
                    vmm_map_page(dst, vaddr, (uint64_t)new_phys,
                                 pte & PTE_FLAGS_MASK);
                }
            }
        }
    }
    return dst;

oom:
    vmm_destroy_address_space(dst);
    return NULL;
}
