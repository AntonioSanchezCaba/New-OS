/*
 * include/kernel/shm.h - Shared memory subsystem
 *
 * Allows processes (and kernel services) to create named memory regions
 * that can be mapped into multiple address spaces simultaneously.
 *
 * Primary use-cases:
 *   1. Compositor window surface buffers (per-window pixel data)
 *   2. IPC message queues
 *   3. Userspace ring buffers (audio, input events)
 *
 * Design:
 *   - Named objects identified by a 64-byte name string
 *   - Physical pages allocated via PMM / buddy allocator
 *   - Reference-counted; destroyed when last mapper unmaps it
 *   - Kernel-side: shm_create / shm_map_kernel for kernel threads
 *   - User-side:   SYS_SHM_OPEN / SYS_SHM_MAP  syscalls
 */
#ifndef KERNEL_SHM_H
#define KERNEL_SHM_H

#include <types.h>

#define SHM_MAX_REGIONS  64
#define SHM_NAME_MAX     64
#define SHM_MAX_PAGES    256   /* Max 1 MB per region */

/* Per-region descriptor */
typedef struct shm_region {
    char      name[SHM_NAME_MAX];
    bool      valid;
    int       refcount;        /* Number of current mappers */
    size_t    size;            /* Byte size (always PAGE_SIZE-aligned) */
    size_t    num_pages;
    uint64_t  phys_pages[SHM_MAX_PAGES]; /* Physical frame addresses */
    int       flags;           /* SHM_READ | SHM_WRITE | SHM_EXEC */
    pid_t     owner;           /* Creating process */
} shm_region_t;

/* Mapping flags */
#define SHM_READ   0x1
#define SHM_WRITE  0x2
#define SHM_EXEC   0x4

/* ── Kernel API ──────────────────────────────────────────────────────── */
void         shm_init(void);

/* Create a new shared region (kernel call) */
shm_region_t* shm_create(const char* name, size_t size, int flags);

/* Open an existing region by name */
shm_region_t* shm_open(const char* name);

/* Map region into the given address space, returns virtual base or 0 */
uint64_t shm_map(shm_region_t* region, void* pml4, uint64_t hint_vaddr,
                  int flags);

/* Map into kernel address space */
void* shm_map_kernel(shm_region_t* region);

/* Unmap and decrement refcount (destroys if last user) */
void shm_unmap(shm_region_t* region, void* pml4, uint64_t vaddr);

/* Destroy immediately (must have refcount == 0) */
void shm_destroy(shm_region_t* region);

/* List all regions (for debugging / sysmonitor) */
void shm_dump(void);

/* Syscall helpers — called from syscall.c */
int64_t sys_shm_open(uint64_t name_addr, uint64_t size, uint64_t flags,
                      uint64_t a4, uint64_t a5, uint64_t a6);
int64_t sys_shm_map(uint64_t shm_id, uint64_t flags,
                     uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6);
int64_t sys_shm_unmap(uint64_t vaddr, uint64_t size,
                       uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6);
int64_t sys_shm_close(uint64_t shm_id,
                       uint64_t a2, uint64_t a3,
                       uint64_t a4, uint64_t a5, uint64_t a6);

#endif /* KERNEL_SHM_H */
