/*
 * kernel/shm.c - Shared memory subsystem
 *
 * Named shared memory regions allow multiple address spaces to access the
 * same physical pages.  Used primarily by the compositor to share window
 * surface buffers with client processes without copying.
 */
#include <kernel/shm.h>
#include <memory.h>
#include <kernel.h>
#include <string.h>
#include <process.h>

/* Global shared memory table */
static shm_region_t shm_table[SHM_MAX_REGIONS];

void shm_init(void)
{
    memset(shm_table, 0, sizeof(shm_table));
    kinfo("SHM: shared memory subsystem initialized (%d slots)", SHM_MAX_REGIONS);
}

/* ── Internal helpers ────────────────────────────────────────────────── */

static shm_region_t* shm_find_by_name(const char* name)
{
    for (int i = 0; i < SHM_MAX_REGIONS; i++) {
        if (shm_table[i].valid &&
            strncmp(shm_table[i].name, name, SHM_NAME_MAX) == 0) {
            return &shm_table[i];
        }
    }
    return NULL;
}

static shm_region_t* shm_alloc_slot(void)
{
    for (int i = 0; i < SHM_MAX_REGIONS; i++) {
        if (!shm_table[i].valid) return &shm_table[i];
    }
    return NULL;
}

/* ── Create ──────────────────────────────────────────────────────────── */

shm_region_t* shm_create(const char* name, size_t size, int flags)
{
    if (!name || size == 0 || size > SHM_MAX_PAGES * PAGE_SIZE) return NULL;

    /* Reject duplicate names */
    if (shm_find_by_name(name)) {
        kwarn("SHM: region '%s' already exists", name);
        return NULL;
    }

    shm_region_t* r = shm_alloc_slot();
    if (!r) {
        kerror("SHM: no free region slots");
        return NULL;
    }

    size = ALIGN_UP(size, PAGE_SIZE);
    size_t npages = size / PAGE_SIZE;

    /* Allocate physical pages */
    for (size_t i = 0; i < npages; i++) {
        void* frame = pmm_alloc_frame();
        if (!frame) {
            /* Roll back already allocated frames */
            for (size_t j = 0; j < i; j++) pmm_free_frame((void*)r->phys_pages[j]);
            kerror("SHM: out of memory allocating '%s'", name);
            return NULL;
        }
        r->phys_pages[i] = (uint64_t)frame;
    }

    strncpy(r->name, name, SHM_NAME_MAX - 1);
    r->size      = size;
    r->num_pages = npages;
    r->flags     = flags;
    r->refcount  = 0;
    r->owner     = current_process ? current_process->pid : 0;
    r->valid     = true;

    /* Zero out the pages */
    for (size_t i = 0; i < npages; i++) {
        memset(PHYS_TO_VIRT(r->phys_pages[i]), 0, PAGE_SIZE);
    }

    kdebug("SHM: created '%s' %zu bytes, %zu pages", name, size, npages);
    return r;
}

/* ── Open (by name) ──────────────────────────────────────────────────── */

shm_region_t* shm_open(const char* name)
{
    if (!name) return NULL;
    shm_region_t* r = shm_find_by_name(name);
    if (!r) kdebug("SHM: region '%s' not found", name);
    return r;
}

/* ── Map into a process address space ────────────────────────────────── */

uint64_t shm_map(shm_region_t* region, void* pml4, uint64_t hint_vaddr, int flags)
{
    if (!region || !pml4) return 0;

    /* Find a suitable virtual address if no hint provided */
    uint64_t vaddr = hint_vaddr;
    if (!vaddr) {
        /* Start scanning from USER_SPACE_START + 256 MB */
        vaddr = USER_SPACE_START + 0x10000000ULL;
    }
    vaddr = ALIGN_UP(vaddr, PAGE_SIZE);

    uint64_t pte_flags = PTE_PRESENT | PTE_USER;
    if (flags & SHM_WRITE) pte_flags |= PTE_WRITABLE;
    if (!(flags & SHM_EXEC)) pte_flags |= PTE_NX;

    for (size_t i = 0; i < region->num_pages; i++) {
        int ret = vmm_map_page((page_table_t*)pml4,
                                vaddr + i * PAGE_SIZE,
                                region->phys_pages[i],
                                pte_flags);
        if (ret != 0) {
            /* Unmap what we already mapped */
            for (size_t j = 0; j < i; j++) {
                vmm_unmap_page((page_table_t*)pml4, vaddr + j * PAGE_SIZE);
            }
            kerror("SHM: vmm_map_page failed for '%s'", region->name);
            return 0;
        }
    }

    region->refcount++;
    kdebug("SHM: mapped '%s' at 0x%llx (refcount=%d)",
           region->name, vaddr, region->refcount);
    return vaddr;
}

/* ── Map into kernel address space ───────────────────────────────────── */

void* shm_map_kernel(shm_region_t* region)
{
    if (!region) return NULL;

    /* For kernel mappings use the PHYS_TO_VIRT macro — pages are already
       mapped in the kernel's direct-physical window at KERNEL_VMA_BASE. */
    region->refcount++;
    return PHYS_TO_VIRT(region->phys_pages[0]);
}

/* ── Unmap ───────────────────────────────────────────────────────────── */

void shm_unmap(shm_region_t* region, void* pml4, uint64_t vaddr)
{
    if (!region) return;

    if (pml4) {
        for (size_t i = 0; i < region->num_pages; i++) {
            vmm_unmap_page((page_table_t*)pml4, vaddr + i * PAGE_SIZE);
        }
    }

    if (region->refcount > 0) region->refcount--;

    if (region->refcount == 0) {
        kdebug("SHM: destroying '%s' (no more refs)", region->name);
        shm_destroy(region);
    }
}

/* ── Destroy ─────────────────────────────────────────────────────────── */

void shm_destroy(shm_region_t* region)
{
    if (!region || !region->valid) return;

    for (size_t i = 0; i < region->num_pages; i++) {
        pmm_free_frame((void*)region->phys_pages[i]);
    }

    memset(region, 0, sizeof(*region));
}

/* ── Debug dump ──────────────────────────────────────────────────────── */

void shm_dump(void)
{
    kinfo("SHM dump:");
    for (int i = 0; i < SHM_MAX_REGIONS; i++) {
        if (!shm_table[i].valid) continue;
        kinfo("  [%d] '%s'  size=%zu  refs=%d  owner=%u",
              i, shm_table[i].name, shm_table[i].size,
              shm_table[i].refcount, shm_table[i].owner);
    }
}

/* ── Syscall entry points ────────────────────────────────────────────── */

int64_t sys_shm_open(uint64_t name_addr, uint64_t size, uint64_t flags,
                      uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    const char* name = (const char*)name_addr;
    if (!name) return -EFAULT;

    shm_region_t* r = shm_find_by_name(name);
    if (!r) {
        if (size == 0) return -ENOENT;
        r = shm_create(name, (size_t)size, (int)flags);
        if (!r) return -ENOMEM;
    }

    /* Return the index in the table as an ID */
    return (int64_t)(r - shm_table);
}

int64_t sys_shm_map(uint64_t shm_id, uint64_t flags,
                     uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (shm_id >= SHM_MAX_REGIONS || !shm_table[shm_id].valid) return -EINVAL;
    if (!current_process) return -ESRCH;

    uint64_t vaddr = shm_map(&shm_table[shm_id],
                              current_process->address_space,
                              0, (int)flags);
    return vaddr ? (int64_t)vaddr : -ENOMEM;
}

int64_t sys_shm_unmap(uint64_t vaddr, uint64_t size,
                       uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)size; (void)a3; (void)a4; (void)a5; (void)a6;
    if (!current_process) return -ESRCH;

    /* Find region by scanning for a mapping at this vaddr */
    for (int i = 0; i < SHM_MAX_REGIONS; i++) {
        if (!shm_table[i].valid) continue;
        uint64_t phys = vmm_get_physical(current_process->address_space, vaddr);
        if (phys && phys == shm_table[i].phys_pages[0]) {
            shm_unmap(&shm_table[i], current_process->address_space, vaddr);
            return 0;
        }
    }
    return -EINVAL;
}

int64_t sys_shm_close(uint64_t shm_id,
                       uint64_t a2, uint64_t a3,
                       uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    if (shm_id >= SHM_MAX_REGIONS || !shm_table[shm_id].valid) return -EINVAL;

    shm_region_t* r = &shm_table[shm_id];
    if (r->refcount <= 1) shm_destroy(r);
    else r->refcount--;

    return 0;
}
