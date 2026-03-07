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
 * Raw framebuffer info — set by fb_raw_setup() after vmm_init() maps 0-4 GB.
 * Used by fb_paint_panic() so panics are visible in graphical mode even
 * before the full framebuffer driver (back buffer, etc.) is initialised.
 */
static volatile uint32_t* raw_vram   = NULL;
static uint32_t           raw_width  = 0;
static uint32_t           raw_height = 0;
static uint32_t           raw_stride = 0; /* pixels per row */

/*
 * fb_raw_setup - store physical framebuffer pointer for panic use.
 * Called from kernel.c immediately after vmm_init() maps 0-4 GB as identity
 * so the physical address can be used as a virtual pointer safely.
 */
void fb_raw_setup(uintptr_t phys, uint32_t w, uint32_t h, uint32_t pitch)
{
    raw_vram   = (volatile uint32_t*)phys;
    raw_width  = w;
    raw_height = h;
    raw_stride = pitch / 4;
}

/*
 * fb_paint_panic - fill the physical framebuffer with a solid colour.
 * Safe to call from kernel_panic() at any point after fb_raw_setup().
 * Does not use the back buffer or any heap allocation.
 */
void fb_paint_panic(uint32_t colour)
{
    if (!raw_vram || !raw_width || !raw_height) return;
    for (uint32_t y = 0; y < raw_height; y++)
        for (uint32_t x = 0; x < raw_width; x++)
            raw_vram[y * raw_stride + x] = colour;
    __asm__ volatile("mfence" ::: "memory");
}

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

    /* Map the physical framebuffer into kernel virtual address space.
     *
     * vmm_init() installs a 0–4 GB identity map (virtual == physical) using
     * 2 MB huge pages.  The framebuffer physical address (e.g. 0xFD000000 ≈
     * 3.95 GB for QEMU Bochs VBE) falls within that range, so we can use the
     * physical address directly as a virtual address — no extra vmm_map_page
     * call is needed and v86 correctly intercepts writes to the physical MMIO
     * region.
     */
    fb.phys_addr = (uint32_t*)(uintptr_t)phys;
    fb.width     = w;
    fb.height    = h;
    fb.pitch     = pitch;
    fb.bpp       = bpp;

    /* Remap the physical framebuffer MMIO pages as uncached (Write-Through +
     * Cache-Disable) so writes bypass the CPU data cache and immediately
     * reach the display device.  Uses 4KB pages — v86/copy.sh does not
     * support 2MB huge-page PDEs in IA-32e mode.                          */
    {
        uint64_t _p   = phys & ~(uint64_t)0xFFF;       /* 4 KB align down */
        uint64_t _end = ((phys + (uint64_t)pitch * h) + 0xFFF) & ~(uint64_t)0xFFF;
        for (; _p < _end && _p < 0x100000000ULL; _p += PAGE_SIZE) {
            vmm_map_page(kernel_pml4, _p, _p,
                         PTE_PRESENT | PTE_WRITABLE | PTE_GLOBAL |
                         PTE_CACHE_DISABLE | PTE_WRITE_THROUGH);
        }
        kinfo("framebuffer: MMIO pages remapped as write-through/uncached (4KB)");
    }

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
 * _fb_blit_rows - blit a range of rows from back_buf to physical fb.
 */
static void _fb_blit_rows(uint32_t row_start, uint32_t row_end,
                            int col_start, int col_w)
{
    for (uint32_t row = row_start; row < row_end && row < fb.height; row++) {
        if (fb.bpp == 32) {
            uint8_t* dst = (uint8_t*)fb.phys_addr +
                           row * fb.pitch + col_start * 4;
            const uint32_t* src = fb.back_buf + row * fb.width + col_start;
            memcpy(dst, src, (size_t)col_w * 4);
        } else {
            uint8_t* dst = (uint8_t*)fb.phys_addr +
                           row * fb.pitch + col_start * 3;
            const uint32_t* src = fb.back_buf + row * fb.width + col_start;
            for (int col = 0; col < col_w; col++) {
                uint32_t px = src[col];
                dst[col*3+0] = (px      ) & 0xFF;
                dst[col*3+1] = (px >>  8) & 0xFF;
                dst[col*3+2] = (px >> 16) & 0xFF;
            }
        }
    }
}

/*
 * fb_flip - copy the full back buffer to the physical framebuffer.
 */
void fb_flip(void)
{
    if (!fb.initialized) return;

    if (fb.bpp == 32 && fb.pitch == fb.width * 4) {
        /* Fast path: single bulk copy */
        memcpy(fb.phys_addr, fb.back_buf,
               (size_t)fb.width * fb.height * 4);
    } else {
        _fb_blit_rows(0, fb.height, 0, (int)fb.width);
    }

    /* Memory fence: ensure all VRAM stores are globally visible before we
     * continue.  Critical when MMIO pages are write-through/uncached so the
     * CPU store buffer is fully drained before the next frame starts.       */
    __asm__ volatile("mfence" ::: "memory");

    fb.frame_count++;
    fb.damage_count = 0;
    fb.full_damage  = false;
}

/* ── Damage tracking ─────────────────────────────────────────────────── */

void fb_damage(int x, int y, int w, int h)
{
    if (!fb.initialized || fb.full_damage) return;
    if (fb.damage_count >= FB_MAX_DAMAGE) { fb.full_damage = true; return; }

    /* Clamp */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)fb.width)  w = (int)fb.width  - x;
    if (y + h > (int)fb.height) h = (int)fb.height - y;
    if (w <= 0 || h <= 0) return;

    fb.damage[fb.damage_count].x = x;
    fb.damage[fb.damage_count].y = y;
    fb.damage[fb.damage_count].w = w;
    fb.damage[fb.damage_count].h = h;
    fb.damage_count++;
}

void fb_damage_full(void) { fb.full_damage = true; fb.damage_count = 0; }
void fb_damage_clear(void) { fb.damage_count = 0; fb.full_damage = false; }

/*
 * fb_flip_damage - flip only dirty rectangles to minimize memory bus traffic.
 * Falls back to full flip when full_damage is set or no damage rects exist.
 */
void fb_flip_damage(void)
{
    if (!fb.initialized) return;

    if (fb.full_damage || fb.damage_count == 0) {
        fb_flip();
        return;
    }

    for (int d = 0; d < fb.damage_count; d++) {
        int rx = fb.damage[d].x;
        int ry = fb.damage[d].y;
        int rw = fb.damage[d].w;
        int rh = fb.damage[d].h;
        _fb_blit_rows((uint32_t)ry, (uint32_t)(ry + rh), rx, rw);
    }

    fb.frame_count++;
    fb.damage_count = 0;
    fb.full_damage  = false;
}

uint64_t fb_frame_count(void) { return fb.frame_count; }

/*
 * fb_blit_alpha - blit with per-pixel alpha blending onto back buffer.
 */
void fb_blit_alpha(int dst_x, int dst_y,
                    const uint32_t* src, int src_w, int src_h, int src_pitch)
{
    for (int row = 0; row < src_h; row++) {
        int dy = dst_y + row;
        if (dy < 0 || (uint32_t)dy >= fb.height) continue;
        for (int col = 0; col < src_w; col++) {
            int dx = dst_x + col;
            if (dx < 0 || (uint32_t)dx >= fb.width) continue;
            uint32_t s = src[row * src_pitch + col];
            uint8_t  a = (s >> 24) & 0xFF;
            if (a == 0) continue;
            uint32_t d = fb.back_buf[dy * fb.width + dx];
            fb.back_buf[dy * fb.width + dx] =
                (a == 0xFF) ? s : fb_blend(d, s);
        }
    }
}

/*
 * fb_init_backbuffer - re-allocate/re-initialize back buffer.
 * Called by uefi_gop_apply() after framebuffer geometry changes.
 */
void fb_init_backbuffer(void)
{
    if (fb.back_buf) {
        kfree(fb.back_buf);
        fb.back_buf = NULL;
    }
    size_t sz = (size_t)fb.width * fb.height * sizeof(uint32_t);
    fb.back_buf = (uint32_t*)kmalloc(sz);
    if (!fb.back_buf) {
        kpanic("framebuffer: fb_init_backbuffer OOM (%u bytes)", (uint32_t)sz);
    }
    memset(fb.back_buf, 0, sz);
}
