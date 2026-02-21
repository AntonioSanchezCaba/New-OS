/*
 * mm/buddy.c — Aether OS Buddy System Allocator
 *
 * A classic binary buddy allocator.
 * Complements the existing PMM bitmap; intended for large contiguous
 * allocations (compositor surface buffers, DMA regions, etc.).
 *
 * Algorithm:
 *   Maintain one doubly-linked free list per order 0..BUDDY_MAX_ORDER.
 *   Alloc of order N:
 *     1. If free[N] has a block, pop it and return it.
 *     2. Otherwise, recursively alloc order N+1 and split: return one
 *        half, push the other half onto free[N].
 *   Free of (addr, order):
 *     1. Compute buddy address (XOR the order bit of the page index).
 *     2. If the buddy is also free, coalesce and free at order N+1.
 *     3. Otherwise, push addr onto free[N].
 *
 * For now the allocator is seeded at init time from a contiguous
 * physical region reported by the existing PMM; it is separate from
 * the kmalloc heap.
 */
#include <mm/buddy.h>
#include <memory.h>
#include <string.h>
#include <kernel.h>

buddy_allocator_t g_buddy;

/* =========================================================
 * Internal helpers
 * ========================================================= */

/* Page index from virtual address */
static inline uint64_t _page_idx(uint64_t virt_addr)
{
    return (virt_addr - g_buddy.base_virt) / BUDDY_PAGE_SIZE;
}

/* Virtual address of a page index */
static inline uint64_t _page_addr(uint64_t idx)
{
    return g_buddy.base_virt + idx * BUDDY_PAGE_SIZE;
}

/* Buddy of page index P at order N */
static inline uint64_t _buddy_idx(uint64_t p, int order)
{
    return p ^ ((uint64_t)1 << order);
}

/* Push a page-index onto free[order] */
static void _push(int order, uint64_t idx)
{
    buddy_node_t* node = (buddy_node_t*)_page_addr(idx);
    node->next = g_buddy.free[order].head;
    node->prev = NULL;
    if (g_buddy.free[order].head)
        g_buddy.free[order].head->prev = node;
    g_buddy.free[order].head = node;
    g_buddy.free[order].count++;
    g_buddy.order_map[idx] = (uint8_t)order;
}

/* Pop a page-index from free[order] (returns BUDDY_MAX_PAGES on fail) */
static uint64_t _pop(int order)
{
    buddy_node_t* node = g_buddy.free[order].head;
    if (!node) return BUDDY_MAX_PAGES;

    g_buddy.free[order].head = node->next;
    if (node->next) node->next->prev = NULL;
    g_buddy.free[order].count--;

    uint64_t idx = _page_idx((uint64_t)(uintptr_t)node);
    g_buddy.order_map[idx] = 0xFF;  /* Sentinel: "allocated" */
    return idx;
}

/* Remove a specific page-index from free[order] */
static void _remove(int order, uint64_t idx)
{
    buddy_node_t* node = (buddy_node_t*)_page_addr(idx);

    if (node->prev) node->prev->next = node->next;
    else            g_buddy.free[order].head = node->next;
    if (node->next) node->next->prev = node->prev;

    g_buddy.free[order].count--;
    g_buddy.order_map[idx] = 0xFF;
}

/* Is page-index in the free list at order N? */
static bool _is_free(uint64_t idx, int order)
{
    return (idx < g_buddy.total_pages &&
            g_buddy.order_map[idx] == (uint8_t)order);
}

/* =========================================================
 * Initialisation
 * ========================================================= */

void buddy_init(uint64_t base_phys, uint64_t base_virt, uint64_t size_bytes)
{
    memset(&g_buddy, 0, sizeof(g_buddy));

    g_buddy.base_phys   = base_phys;
    g_buddy.base_virt   = base_virt;
    g_buddy.total_pages = size_bytes / BUDDY_PAGE_SIZE;
    g_buddy.free_pages  = 0;
    g_buddy.initialized = false;

    if (g_buddy.total_pages > BUDDY_MAX_PAGES)
        g_buddy.total_pages = BUDDY_MAX_PAGES;

    /* order_map lives in the first pages of the managed region */
    g_buddy.order_map = (uint8_t*)base_virt;
    uint64_t map_pages = (g_buddy.total_pages + BUDDY_PAGE_SIZE - 1)
                          / BUDDY_PAGE_SIZE;
    memset(g_buddy.order_map, 0xFF, g_buddy.total_pages); /* all "allocated" */

    /* Free all pages beyond the order_map itself at the highest order */
    uint64_t start_page = map_pages;
    uint64_t npages     = g_buddy.total_pages - start_page;

    /* Seed: free pages from largest aligned blocks downward */
    uint64_t p = start_page;
    for (int ord = BUDDY_MAX_ORDER; ord >= 0 && p < start_page + npages; ord--) {
        uint64_t blk = (uint64_t)1 << ord;
        /* Align p to this block size */
        uint64_t aligned_p = (p + blk - 1) & ~(blk - 1);
        while (aligned_p + blk <= start_page + npages) {
            _push(ord, aligned_p);
            g_buddy.free_pages += blk;
            aligned_p += blk;
            p = aligned_p;
        }
    }

    g_buddy.initialized = true;

    kinfo("BUDDY: initialized — base=0x%llx size=%llu KB "
          "(%llu pages, %llu free)",
          (unsigned long long)base_phys,
          (unsigned long long)(size_bytes / 1024),
          (unsigned long long)g_buddy.total_pages,
          (unsigned long long)g_buddy.free_pages);
}

/* =========================================================
 * buddy_alloc — allocate 2^order contiguous pages
 * Returns virtual address of the first page, or 0 on failure.
 * ========================================================= */

uint64_t buddy_alloc(int order)
{
    if (!g_buddy.initialized) return 0;
    if (order < 0 || order > BUDDY_MAX_ORDER) return 0;

    /* Find the smallest order with a free block */
    int avail = order;
    while (avail <= BUDDY_MAX_ORDER && g_buddy.free[avail].count == 0)
        avail++;

    if (avail > BUDDY_MAX_ORDER) return 0;  /* Out of memory */

    /* Pop the block at avail and split down to the requested order */
    uint64_t idx = _pop(avail);

    while (avail > order) {
        avail--;
        /* The upper half becomes a free buddy */
        uint64_t buddy = idx + ((uint64_t)1 << avail);
        _push(avail, buddy);
    }

    uint64_t pages = (uint64_t)1 << order;
    g_buddy.free_pages -= pages;

    return _page_addr(idx);
}

/* =========================================================
 * buddy_free — return a block of 2^order pages
 * ========================================================= */

void buddy_free(uint64_t virt_addr, int order)
{
    if (!g_buddy.initialized) return;
    if (order < 0 || order > BUDDY_MAX_ORDER) return;

    uint64_t idx = _page_idx(virt_addr);
    if (idx >= g_buddy.total_pages) return;

    /* Coalesce with buddy while possible */
    while (order < BUDDY_MAX_ORDER) {
        uint64_t buddy = _buddy_idx(idx, order);
        if (!_is_free(buddy, order)) break;

        /* Remove buddy from its free list and merge */
        _remove(order, buddy);
        if (buddy < idx) idx = buddy;   /* Take the lower address */
        order++;
    }

    _push(order, idx);
    g_buddy.free_pages += (uint64_t)1 << order;
}

/* =========================================================
 * Diagnostics
 * ========================================================= */

void buddy_dump_stats(void)
{
    kinfo("BUDDY: %llu / %llu pages free (%llu KB)",
          (unsigned long long)g_buddy.free_pages,
          (unsigned long long)g_buddy.total_pages,
          (unsigned long long)(g_buddy.free_pages * BUDDY_PAGE_SIZE / 1024));

    for (int i = 0; i <= BUDDY_MAX_ORDER; i++) {
        if (g_buddy.free[i].count == 0) continue;
        kinfo("  order[%2d]: %4u blocks × %4u pages = %6u KB",
              i, g_buddy.free[i].count,
              (1u << i),
              (g_buddy.free[i].count * (1u << i) * BUDDY_PAGE_SIZE / 1024));
    }
}

uint64_t buddy_free_bytes(void)
{
    return g_buddy.free_pages * BUDDY_PAGE_SIZE;
}
