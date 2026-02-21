/*
 * memory/heap.c - Kernel heap allocator (kmalloc / kfree)
 *
 * Implements a free-list heap allocator with block headers.
 * The heap occupies a region of kernel virtual address space
 * (KERNEL_HEAP_START to KERNEL_HEAP_START + size).
 *
 * Block layout:
 *   [heap_block_t header | user data area]
 *
 * Free blocks are stored in a singly-linked free list, sorted by address.
 * On free, adjacent free blocks are coalesced.
 */
#include <memory.h>
#include <kernel.h>
#include <types.h>
#include <string.h>

/* Minimum allocation size (to avoid tiny fragmented blocks) */
#define HEAP_MIN_ALLOC  16
#define HEAP_MAGIC      0xDEADBEEF

/* Block header - placed immediately before every allocation */
typedef struct heap_block {
    uint32_t        magic;   /* HEAP_MAGIC - sanity check */
    uint32_t        flags;   /* bit 0 = free, bit 1 = last block */
    size_t          size;    /* Size of the DATA area (not including header) */
    struct heap_block* next; /* Next block in free list (only valid if free) */
} heap_block_t;

#define BLOCK_FREE  (1 << 0)
#define BLOCK_LAST  (1 << 1)

/* Header size (aligned to 16 bytes for natural alignment of returned pointers) */
#define HDR_SIZE    (ALIGN_UP(sizeof(heap_block_t), 16))

/* Heap state */
static uint64_t heap_start_addr = 0;
static uint64_t heap_end_addr   = 0;
static uint64_t heap_current    = 0; /* Current break */
static heap_block_t* free_list  = NULL;
static size_t heap_used_bytes   = 0;

/* ============================================================
 * Internal helpers
 * ============================================================ */

/* Given a block header, return a pointer to its data area */
static inline void* block_to_ptr(heap_block_t* block) {
    return (void*)((uint8_t*)block + HDR_SIZE);
}

/* Given a pointer to a data area, return its block header */
static inline heap_block_t* ptr_to_block(void* ptr) {
    return (heap_block_t*)((uint8_t*)ptr - HDR_SIZE);
}

/* Return the block that immediately follows this one in memory */
static inline heap_block_t* next_block(heap_block_t* block) {
    return (heap_block_t*)((uint8_t*)block + HDR_SIZE + block->size);
}

/*
 * expand_heap - grow the heap by at least @min_size bytes.
 * Maps new physical pages into the heap virtual address range.
 */
static int expand_heap(size_t min_size)
{
    size_t needed = ALIGN_UP(min_size + HDR_SIZE, PAGE_SIZE);

    if (heap_current + needed > heap_end_addr) {
        return -1; /* Heap exhausted */
    }

    /* Map new physical pages */
    for (uint64_t addr = heap_current; addr < heap_current + needed; addr += PAGE_SIZE) {
        void* phys = pmm_alloc_frame();
        if (!phys) return -1;
        vmm_map_page(kernel_pml4, addr, (uint64_t)phys,
                     PTE_PRESENT | PTE_WRITABLE | PTE_GLOBAL);
    }

    /* Create a free block covering the new region */
    heap_block_t* block = (heap_block_t*)heap_current;
    block->magic = HEAP_MAGIC;
    block->flags = BLOCK_FREE | BLOCK_LAST;
    block->size  = needed - HDR_SIZE;
    block->next  = NULL;

    heap_current += needed;

    /* Insert into free list (at head, sorted order not required for basic ops) */
    block->next = free_list;
    free_list = block;

    return 0;
}

/* ============================================================
 * Public API
 * ============================================================ */

void heap_init(uint64_t start, size_t size)
{
    heap_start_addr = start;
    heap_end_addr   = start + size;
    heap_current    = start;
    free_list       = NULL;
    heap_used_bytes = 0;

    /* Pre-allocate initial 1MB of heap */
    expand_heap(1024 * 1024);
    kinfo("Heap: start=0x%llx, size=%u MB", start, (uint32_t)(size / (1024*1024)));
}

/*
 * kmalloc - allocate @size bytes from the kernel heap.
 * Returns: 16-byte aligned pointer, or NULL on failure.
 */
void* kmalloc(size_t size)
{
    if (size == 0) return NULL;
    size = ALIGN_UP(MAX(size, HEAP_MIN_ALLOC), 16);

    /* Search the free list for a suitable block (first-fit) */
    heap_block_t** prev = &free_list;
    heap_block_t*  curr = free_list;

    while (curr) {
        if (curr->magic != HEAP_MAGIC) {
            kpanic("Heap corruption detected at %p (magic=0x%x)",
                   (void*)curr, curr->magic);
        }

        if ((curr->flags & BLOCK_FREE) && curr->size >= size) {
            /* Found a free block large enough */
            size_t leftover = curr->size - size;

            if (leftover >= HDR_SIZE + HEAP_MIN_ALLOC) {
                /* Split the block */
                heap_block_t* new_block =
                    (heap_block_t*)((uint8_t*)curr + HDR_SIZE + size);
                new_block->magic = HEAP_MAGIC;
                new_block->flags = BLOCK_FREE | (curr->flags & BLOCK_LAST);
                new_block->size  = leftover - HDR_SIZE;
                new_block->next  = curr->next;

                curr->size  = size;
                curr->flags &= ~BLOCK_LAST;
                curr->next  = new_block;

                /* The new block takes curr's place in the free list */
                *prev = new_block;
            } else {
                /* Use entire block */
                *prev = curr->next;
            }

            curr->flags &= ~BLOCK_FREE;
            heap_used_bytes += curr->size;
            return block_to_ptr(curr);
        }

        prev = &curr->next;
        curr = curr->next;
    }

    /* No suitable block found - expand heap */
    if (expand_heap(size) < 0) {
        kerror("kmalloc: out of kernel heap memory (requested %u bytes)", (uint32_t)size);
        return NULL;
    }

    /* Retry after expansion */
    return kmalloc(size);
}

/*
 * kmalloc_aligned - allocate @size bytes with @align-byte alignment.
 * Simple implementation: over-allocate and adjust.
 */
void* kmalloc_aligned(size_t size, size_t align)
{
    if (align <= 16) return kmalloc(size);

    /* Allocate extra space for alignment padding + pointer storage */
    void* raw = kmalloc(size + align + sizeof(void*));
    if (!raw) return NULL;

    /* Align upward */
    uintptr_t aligned = ALIGN_UP((uintptr_t)raw + sizeof(void*), align);

    /* Store original pointer just before the aligned region */
    ((void**)aligned)[-1] = raw;

    return (void*)aligned;
}

/*
 * kcalloc - allocate and zero @count * @size bytes.
 */
void* kcalloc(size_t count, size_t size)
{
    size_t total = count * size;
    void* ptr = kmalloc(total);
    if (ptr) memset(ptr, 0, total);
    return ptr;
}

/*
 * krealloc - resize an allocation.
 */
void* krealloc(void* ptr, size_t new_size)
{
    if (!ptr) return kmalloc(new_size);
    if (new_size == 0) { kfree(ptr); return NULL; }

    heap_block_t* block = ptr_to_block(ptr);
    if (block->magic != HEAP_MAGIC) {
        kpanic("krealloc: invalid pointer %p", ptr);
    }

    if (block->size >= new_size) return ptr; /* Already big enough */

    void* new_ptr = kmalloc(new_size);
    if (!new_ptr) return NULL;

    memcpy(new_ptr, ptr, block->size);
    kfree(ptr);
    return new_ptr;
}

/*
 * kfree - release a previously allocated block.
 */
void kfree(void* ptr)
{
    if (!ptr) return;

    heap_block_t* block = ptr_to_block(ptr);
    if (block->magic != HEAP_MAGIC) {
        kpanic("kfree: double-free or invalid pointer %p (magic=0x%x)",
               ptr, block->magic);
    }
    if (block->flags & BLOCK_FREE) {
        kpanic("kfree: double-free detected at %p", ptr);
    }

    heap_used_bytes -= block->size;
    block->flags |= BLOCK_FREE;

    /* Insert at the head of the free list */
    block->next = free_list;
    free_list = block;

    /* Coalesce adjacent free blocks (simple forward pass) */
    heap_block_t* curr = free_list;
    while (curr && !(curr->flags & BLOCK_LAST)) {
        heap_block_t* nxt = next_block(curr);
        if (nxt->magic == HEAP_MAGIC && (nxt->flags & BLOCK_FREE)) {
            /* Merge curr and nxt */
            curr->size  += HDR_SIZE + nxt->size;
            curr->flags  = (curr->flags & ~BLOCK_LAST) | (nxt->flags & BLOCK_LAST);
            curr->next   = nxt->next;
            nxt->magic   = 0; /* Invalidate merged header */
        } else {
            curr = curr->next;
        }
    }
}

size_t kheap_used(void) { return heap_used_bytes; }
size_t kheap_free(void) { return heap_end_addr - heap_current +
                                   (heap_current - heap_start_addr) -
                                   heap_used_bytes; }
