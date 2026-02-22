/*
 * memory/pmm.c - Physical Memory Manager (bitmap allocator)
 *
 * Manages physical memory at page granularity (4KB frames).
 * Uses a bitmap where each bit represents one 4KB frame:
 *   0 = frame is free
 *   1 = frame is in use
 *
 * The bitmap itself is stored in kernel BSS (statically allocated for
 * up to 4GB of physical RAM = 1M frames = 128KB of bitmap space).
 */
#include <memory.h>
#include <multiboot2.h>
#include <kernel.h>
#include <types.h>
#include <string.h>

/* Maximum number of 4KB frames we track (supports up to 4GB of RAM) */
#define MAX_FRAMES  (PMM_MAX_MEMORY / PMM_FRAME_SIZE)

/* Bitmap: 1 bit per frame. MAX_FRAMES / 8 bytes total. */
static uint8_t pmm_bitmap[MAX_FRAMES / 8];

/* Reference counts: 1 byte per frame (saturates at 255). */
static uint8_t pmm_refcounts[MAX_FRAMES];

/* Statistics */
static size_t pmm_total_frame_count = 0;
static size_t pmm_used_frame_count  = 0;

/* Kernel physical start/end (for marking kernel as used) */
extern uint8_t _kernel_phys_start[];
extern uint8_t _kernel_phys_end[];

/* ============================================================
 * Bitmap helpers
 * ============================================================ */

static inline void pmm_set_bit(uint64_t frame)
{
    pmm_bitmap[frame / 8] |= (1 << (frame % 8));
}

static inline void pmm_clear_bit(uint64_t frame)
{
    pmm_bitmap[frame / 8] &= ~(1 << (frame % 8));
}

static inline bool pmm_test_bit(uint64_t frame)
{
    return (pmm_bitmap[frame / 8] >> (frame % 8)) & 1;
}

/* ============================================================
 * Public API
 * ============================================================ */

/*
 * pmm_mark_used - mark a physical address range as in-use.
 * @addr: physical base address (will be page-aligned down)
 * @size: length in bytes (will be page-aligned up)
 */
void pmm_mark_used(uint64_t addr, size_t size)
{
    uint64_t frame_start = addr / PMM_FRAME_SIZE;
    uint64_t frame_end   = ALIGN_UP(addr + size, PMM_FRAME_SIZE) / PMM_FRAME_SIZE;

    for (uint64_t f = frame_start; f < frame_end && f < MAX_FRAMES; f++) {
        if (!pmm_test_bit(f)) {
            pmm_set_bit(f);
            pmm_used_frame_count++;
        }
    }
}

/*
 * pmm_mark_free - mark a physical address range as available.
 */
void pmm_mark_free(uint64_t addr, size_t size)
{
    uint64_t frame_start = ALIGN_UP(addr, PMM_FRAME_SIZE) / PMM_FRAME_SIZE;
    uint64_t frame_end   = (addr + size) / PMM_FRAME_SIZE;

    for (uint64_t f = frame_start; f < frame_end && f < MAX_FRAMES; f++) {
        if (pmm_test_bit(f)) {
            pmm_clear_bit(f);
            pmm_used_frame_count--;
        }
    }
}

/*
 * pmm_init - initialize the PMM from Multiboot2 memory map.
 *
 * Strategy:
 *   1. Mark all frames as used (safe default)
 *   2. Parse the Multiboot2 memory map and free available regions
 *   3. Re-mark special regions as used:
 *      a) Low memory (below 1MB) - has BIOS, VGA, etc.
 *      b) Kernel image itself
 *      c) Page tables set up by boot.asm
 *      d) The PMM bitmap itself
 */
void pmm_init(struct multiboot2_info* mb2_info)
{
    /* Step 1: Mark all frames as used */
    memset(pmm_bitmap, 0xFF, sizeof(pmm_bitmap));
    pmm_used_frame_count = MAX_FRAMES;

    /* Step 2: Parse Multiboot2 memory map */
    struct multiboot2_tag_mmap* mmap_tag =
        (struct multiboot2_tag_mmap*)multiboot2_find_tag(mb2_info,
                                                          MULTIBOOT2_TAG_MMAP);
    if (!mmap_tag) {
        kpanic("No Multiboot2 memory map found!");
    }

    /* Iterate over memory map entries */
    uint8_t* entry_ptr  = (uint8_t*)mmap_tag->entries;
    uint8_t* mmap_end   = (uint8_t*)mmap_tag + mmap_tag->size;

    while (entry_ptr < mmap_end) {
        struct multiboot2_mmap_entry* entry =
            (struct multiboot2_mmap_entry*)entry_ptr;

        if (entry->type == MULTIBOOT2_MMAP_AVAILABLE) {
            /* Free this region */
            pmm_mark_free(entry->base_addr, entry->length);
            pmm_total_frame_count +=
                ALIGN_DOWN(entry->length, PMM_FRAME_SIZE) / PMM_FRAME_SIZE;
        }

        entry_ptr += mmap_tag->entry_size;
    }

    /* Step 3a: Re-mark low memory (0 - 1MB) as used
     *          (BIOS, VGA buffer, IVT, boot page tables at 0x1000-0x5000, etc.)
     */
    pmm_mark_used(0, 0x100000);

    /* Step 3b: Re-mark the kernel image as used
     *          The kernel is physically loaded at KERNEL_PHYS_BASE.
     *          _kernel_phys_end is provided by the linker script.
     */
    uint64_t kstart = KERNEL_PHYS_BASE;
    uint64_t kend   = (uint64_t)_kernel_phys_end;
    pmm_mark_used(kstart, kend - kstart);

    /* Step 3c: Re-mark the PMM bitmap itself as used */
    pmm_mark_used(VIRT_TO_PHYS((uint64_t)pmm_bitmap), sizeof(pmm_bitmap));
}

/*
 * pmm_alloc_frame - allocate a single 4KB physical frame.
 *
 * Returns the physical address of the frame, or NULL if out of memory.
 * Simple linear scan (can be optimized with a buddy system or free list).
 * The frame's reference count is initialised to 1.
 */
void* pmm_alloc_frame(void)
{
    /* Search for the first free bit in the bitmap */
    for (size_t byte = 0; byte < sizeof(pmm_bitmap); byte++) {
        if (pmm_bitmap[byte] == 0xFF) continue; /* All 8 bits used */

        for (int bit = 0; bit < 8; bit++) {
            uint64_t frame = byte * 8 + bit;
            if (!pmm_test_bit(frame)) {
                pmm_set_bit(frame);
                pmm_refcounts[frame] = 1;
                pmm_used_frame_count++;
                return (void*)(frame * PMM_FRAME_SIZE);
            }
        }
    }

    return NULL; /* Out of physical memory */
}

/*
 * pmm_alloc_frames - allocate @count contiguous physical frames.
 *
 * Returns the base physical address, or NULL if not enough contiguous memory.
 * Each allocated frame's reference count is initialised to 1.
 */
void* pmm_alloc_frames(size_t count)
{
    if (count == 0) return NULL;
    if (count == 1) return pmm_alloc_frame();

    size_t run = 0;      /* Current run of free frames */
    size_t start = 0;    /* Start of current run */

    for (size_t f = 0; f < MAX_FRAMES; f++) {
        if (!pmm_test_bit(f)) {
            if (run == 0) start = f;
            run++;
            if (run == count) {
                /* Found a contiguous run - mark them all used */
                for (size_t i = start; i < start + count; i++) {
                    pmm_set_bit(i);
                    pmm_refcounts[i] = 1;
                    pmm_used_frame_count++;
                }
                return (void*)(start * PMM_FRAME_SIZE);
            }
        } else {
            run = 0;
        }
    }

    return NULL;
}

/*
 * pmm_free_frame - release a previously allocated 4KB physical frame.
 * @frame: physical address (must be page-aligned)
 *
 * Decrements the reference count; only actually frees when it reaches 0.
 */
void pmm_free_frame(void* frame)
{
    uint64_t f = (uint64_t)frame / PMM_FRAME_SIZE;
    if (f >= MAX_FRAMES) return;

    if (!pmm_test_bit(f)) return; /* Already free */

    if (pmm_refcounts[f] > 1) {
        pmm_refcounts[f]--;
        return; /* Still referenced by another mapping */
    }

    pmm_refcounts[f] = 0;
    pmm_clear_bit(f);
    pmm_used_frame_count--;
}

/*
 * pmm_free_frames - release @count contiguous frames starting at @frame.
 */
void pmm_free_frames(void* frame, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        pmm_free_frame((void*)((uint64_t)frame + i * PMM_FRAME_SIZE));
    }
}

/* ============================================================
 * Reference counting API (used by COW)
 * ============================================================ */

/*
 * pmm_ref_frame - increment the reference count of a physical frame.
 * Used when a page is shared copy-on-write.
 */
void pmm_ref_frame(uint64_t phys)
{
    uint64_t f = phys / PMM_FRAME_SIZE;
    if (f >= MAX_FRAMES) return;
    if (pmm_refcounts[f] < 255) /* saturate at 255 */
        pmm_refcounts[f]++;
}

/*
 * pmm_unref_frame - decrement the reference count, freeing if it hits 0.
 */
void pmm_unref_frame(uint64_t phys)
{
    pmm_free_frame((void*)phys);
}

/*
 * pmm_get_refcount - return the current reference count of a frame.
 */
uint8_t pmm_get_refcount(uint64_t phys)
{
    uint64_t f = phys / PMM_FRAME_SIZE;
    if (f >= MAX_FRAMES) return 0;
    return pmm_refcounts[f];
}

size_t pmm_free_frames_count(void)
{
    return pmm_total_frame_count - pmm_used_frame_count;
}

size_t pmm_total_frames(void)
{
    return pmm_total_frame_count;
}
