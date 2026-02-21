/*
 * drivers/framebuffer.c - Linear framebuffer driver
 *
 * Initialises from the Multiboot2 framebuffer tag, allocates a back buffer,
 * and provides pixel-level access and a double-buffer flip operation.
 */
#include <drivers/framebuffer.h>
#include <multiboot2.h>
#include <memory.h>
#include <kernel.h>
#include <types.h>
#include <string.h>

framebuffer_t fb = { 0 };

/*
 * fb_init - parse the Multiboot2 framebuffer tag and set up the driver.
 */
void fb_init(struct multiboot2_tag_framebuffer* fb_tag)
{
    if (!fb_tag) {
        kwarn("framebuffer: no framebuffer tag from bootloader");
        return;
    }

    if (fb_tag->framebuffer_type != 1) {
        /* Type 2 = VGA text mode — we need type 1 (RGB linear) */
        kwarn("framebuffer: bootloader did not provide a linear framebuffer "
              "(type=%u). Add 'set gfxpayload=keep' to GRUB config.",
              fb_tag->framebuffer_type);
        return;
    }

    uint64_t phys = fb_tag->framebuffer_addr;
    uint32_t w    = fb_tag->framebuffer_width;
    uint32_t h    = fb_tag->framebuffer_height;
    uint32_t pitch= fb_tag->framebuffer_pitch;
    uint8_t  bpp  = fb_tag->framebuffer_bpp;

    kinfo("Framebuffer: %ux%u %ubpp, pitch=%u, phys=0x%llx", w, h, bpp, pitch, phys);

    if (bpp != 32 && bpp != 24) {
        kwarn("framebuffer: unsupported bpp=%u (need 24 or 32)", bpp);
        return;
    }

    /* Map the physical framebuffer into kernel virtual address space */
    size_t fb_bytes = (size_t)pitch * h;
    size_t pages    = ALIGN_UP(fb_bytes, PAGE_SIZE) / PAGE_SIZE;

    for (size_t p = 0; p < pages; p++) {
        uint64_t paddr = phys + p * PAGE_SIZE;
        /* Mark these frames as used in the PMM (they are device memory) */
        vmm_map_page(kernel_pml4, paddr + KERNEL_VMA_BASE, paddr,
                     PTE_PRESENT | PTE_WRITABLE | PTE_GLOBAL |
                     PTE_CACHE_DISABLE);
    }

    fb.phys_addr = (uint32_t*)(phys + KERNEL_VMA_BASE);
    fb.width     = w;
    fb.height    = h;
    fb.pitch     = pitch;
    fb.bpp       = bpp;

    /* Allocate back buffer in kernel heap */
    fb.back_buf = (uint32_t*)kmalloc(w * h * sizeof(uint32_t));
    if (!fb.back_buf) {
        kpanic("framebuffer: failed to allocate back buffer (%u bytes)",
               w * h * 4u);
    }
    memset(fb.back_buf, 0, w * h * sizeof(uint32_t));

    fb.initialized = true;
    kinfo("Framebuffer ready: %ux%u back buffer at %p", w, h, (void*)fb.back_buf);
}

bool fb_ready(void) { return fb.initialized; }

/* ============================================================
 * Back buffer operations
 * ============================================================ */

void fb_put_pixel(int x, int y, uint32_t color)
{
    if ((unsigned)x >= fb.width || (unsigned)y >= fb.height) return;
    fb.back_buf[y * fb.width + x] = color;
}

uint32_t fb_get_pixel(int x, int y)
{
    if ((unsigned)x >= fb.width || (unsigned)y >= fb.height) return 0;
    return fb.back_buf[y * fb.width + x];
}

void fb_clear(uint32_t color)
{
    uint32_t count = fb.width * fb.height;
    uint32_t* p = fb.back_buf;
    while (count--) *p++ = color;
}

void fb_blit_region(int dst_x, int dst_y,
                    const uint32_t* src, int src_w, int src_h, int src_pitch)
{
    for (int row = 0; row < src_h; row++) {
        int dy = dst_y + row;
        if (dy < 0 || (uint32_t)dy >= fb.height) continue;
        for (int col = 0; col < src_w; col++) {
            int dx = dst_x + col;
            if (dx < 0 || (uint32_t)dx >= fb.width) continue;
            fb.back_buf[dy * fb.width + dx] = src[row * src_pitch + col];
        }
    }
}

/*
 * fb_flip - copy the back buffer to the physical framebuffer (display).
 *
 * For 32bpp: one memcpy per row (fast path when pitch == width * 4).
 * For 24bpp: convert each pixel.
 */
void fb_flip(void)
{
    if (!fb.initialized) return;

    if (fb.bpp == 32 && fb.pitch == fb.width * 4) {
        /* Fast path: single bulk copy */
        memcpy(fb.phys_addr, fb.back_buf,
               (size_t)fb.width * fb.height * 4);
    } else if (fb.bpp == 32) {
        /* Pitch differs from stride — copy row by row */
        for (uint32_t row = 0; row < fb.height; row++) {
            uint8_t* dst = (uint8_t*)fb.phys_addr + row * fb.pitch;
            const uint32_t* src = fb.back_buf + row * fb.width;
            memcpy(dst, src, fb.width * 4);
        }
    } else {
        /* 24bpp: strip the alpha byte */
        for (uint32_t row = 0; row < fb.height; row++) {
            uint8_t* dst = (uint8_t*)fb.phys_addr + row * fb.pitch;
            const uint32_t* src = fb.back_buf + row * fb.width;
            for (uint32_t col = 0; col < fb.width; col++) {
                uint32_t px = src[col];
                dst[col * 3 + 0] = (px      ) & 0xFF; /* B */
                dst[col * 3 + 1] = (px >>  8) & 0xFF; /* G */
                dst[col * 3 + 2] = (px >> 16) & 0xFF; /* R */
            }
        }
    }
}
