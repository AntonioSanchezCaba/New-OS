/*
 * include/mm/buddy.h — Aether OS Buddy System Allocator
 *
 * A binary buddy allocator for physical page frames.
 * Provides O(log n) allocation and deallocation with automatic
 * coalescing of adjacent free blocks.
 *
 * Orders:
 *   Order 0  →  4 KB  (1 page)
 *   Order 1  →  8 KB  (2 pages)
 *   Order 2  →  16 KB (4 pages)
 *   ...
 *   Order 10 →  4 MB  (1024 pages)
 *
 * Used for:
 *   - Compositor surface buffers (large contiguous allocations)
 *   - DMA buffers for hardware drivers
 *   - Large kernel data structures
 *   - Future user-space memory mapping
 */
#ifndef MM_BUDDY_H
#define MM_BUDDY_H

#include <types.h>

#define BUDDY_MAX_ORDER    10           /* Maximum allocation order */
#define BUDDY_PAGE_SIZE    4096         /* One page = 4 KB */
#define BUDDY_MAX_PAGES    65536        /* Up to 256 MB managed */

/* Free-list node embedded in the free page itself */
typedef struct buddy_node {
    struct buddy_node* next;
    struct buddy_node* prev;
} buddy_node_t;

/* Per-order free list */
typedef struct {
    buddy_node_t*  head;     /* First free block of this order */
    uint32_t       count;    /* Number of free blocks at this order */
} buddy_list_t;

/* The buddy allocator state */
typedef struct {
    uint64_t       base_phys;               /* Physical base address */
    uint64_t       base_virt;               /* Virtual (mapped) base address */
    uint64_t       total_pages;             /* Total managed pages */
    uint64_t       free_pages;              /* Currently free pages */
    buddy_list_t   free[BUDDY_MAX_ORDER+1]; /* One list per order */
    uint8_t*       order_map;               /* order_map[page_idx] = current order */
    bool           initialized;
} buddy_allocator_t;

extern buddy_allocator_t g_buddy;

/* =========================================================
 * API
 * ========================================================= */

/* Initialize from the existing PMM's free frame pool */
void      buddy_init(uint64_t base_phys, uint64_t base_virt, uint64_t size_bytes);

/* Allocate 2^order contiguous pages (returns virtual address, or 0) */
uint64_t  buddy_alloc(int order);

/* Free a block previously allocated at the given order */
void      buddy_free(uint64_t virt_addr, int order);

/* Convenience wrappers */
static inline uint64_t buddy_alloc_pages(uint32_t n_pages)
{
    /* Find smallest order that fits n_pages */
    int order = 0;
    uint32_t sz = 1;
    while (sz < n_pages && order < BUDDY_MAX_ORDER) { sz <<= 1; order++; }
    return buddy_alloc(order);
}

/* Diagnostics */
void      buddy_dump_stats(void);
uint64_t  buddy_free_bytes(void);

#endif /* MM_BUDDY_H */
