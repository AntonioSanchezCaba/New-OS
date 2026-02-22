/*
 * drivers/uefi_gop.c — UEFI GOP framebuffer initialisation
 *
 * Multiboot2 spec §3.6: framebuffer info tag (type=8)
 * The bootloader (GRUB) negotiates the graphics mode via UEFI GOP and
 * passes the result as a Multiboot2 framebuffer tag. We parse it here
 * instead of making runtime UEFI calls (which would require being in
 * UEFI boot services context, already exited by the bootloader).
 */
#include <drivers/uefi_gop.h>
#include <drivers/framebuffer.h>
#include <multiboot2.h>
#include <kernel.h>
#include <string.h>
#include <types.h>

/* Multiboot2 framebuffer type values */
#define MB2_FRAMEBUFFER_TYPE_INDEXED 0
#define MB2_FRAMEBUFFER_TYPE_RGB     1
#define MB2_FRAMEBUFFER_TYPE_EGA     2

static bool    g_available = false;
static gop_info_t g_info;

/* =========================================================
 * Multiboot2 tag walker — find framebuffer tag
 * ========================================================= */
int uefi_gop_init(const void* mb2_info, gop_info_t* out)
{
    if (!mb2_info) return -1;

    const uint8_t* ptr = (const uint8_t*)mb2_info;
    uint32_t total_size = *(const uint32_t*)ptr;
    ptr += 8;  /* Skip fixed part: total_size + reserved */

    const uint8_t* end = (const uint8_t*)mb2_info + total_size;

    while (ptr < end) {
        const struct {
            uint32_t type;
            uint32_t size;
        } *tag = (const void*)ptr;

        if (tag->type == 0) break;  /* End tag */

        if (tag->type == 8) {
            /* Framebuffer tag */
            const struct {
                uint32_t type;
                uint32_t size;
                uint64_t framebuffer_addr;
                uint32_t framebuffer_pitch;
                uint32_t framebuffer_width;
                uint32_t framebuffer_height;
                uint8_t  framebuffer_bpp;
                uint8_t  framebuffer_type;
                uint16_t reserved;
                /* Color info follows for type 1 (RGB) */
                uint8_t  fb_red_field_position;
                uint8_t  fb_red_mask_size;
                uint8_t  fb_green_field_position;
                uint8_t  fb_green_mask_size;
                uint8_t  fb_blue_field_position;
                uint8_t  fb_blue_mask_size;
            } *fb_tag = (const void*)ptr;

            if (fb_tag->framebuffer_type == MB2_FRAMEBUFFER_TYPE_RGB &&
                fb_tag->framebuffer_bpp  == 32) {

                out->available    = true;
                out->fb_addr      = fb_tag->framebuffer_addr;
                out->width        = fb_tag->framebuffer_width;
                out->height       = fb_tag->framebuffer_height;
                out->pitch        = fb_tag->framebuffer_pitch;
                out->bpp          = fb_tag->framebuffer_bpp;

                /* Determine pixel format from field positions */
                if (fb_tag->fb_red_field_position   == 16 &&
                    fb_tag->fb_green_field_position == 8  &&
                    fb_tag->fb_blue_field_position  == 0) {
                    out->pixel_format = GOP_PIXEL_BGR;  /* BGRX in memory */
                } else {
                    out->pixel_format = GOP_PIXEL_RGB;
                }

                /* Build bitmasks */
                out->red_mask   = ((1u << fb_tag->fb_red_mask_size)   - 1)
                                  << fb_tag->fb_red_field_position;
                out->green_mask = ((1u << fb_tag->fb_green_mask_size) - 1)
                                  << fb_tag->fb_green_field_position;
                out->blue_mask  = ((1u << fb_tag->fb_blue_mask_size)  - 1)
                                  << fb_tag->fb_blue_field_position;

                g_available = true;
                memcpy(&g_info, out, sizeof(g_info));

                kinfo("GOP: %ux%u @ 32bpp addr=%016llX pitch=%u",
                      out->width, out->height,
                      (unsigned long long)out->fb_addr, out->pitch);
                return 0;
            }
        }

        /* Advance to next tag (8-byte aligned) */
        uint32_t next = (tag->size + 7) & ~7u;
        ptr += next;
    }

    klog_warn("GOP: no 32bpp RGB framebuffer tag found in Multiboot2 info");
    return -1;
}

void uefi_gop_apply(const gop_info_t* info)
{
    if (!info || !info->available) return;

    /* Map the physical framebuffer into our kernel virtual address space.
     * For now (identity-mapped kernel), physical == virtual for low memory.
     * Full UEFI memory map handling would remap via VMM. */
    fb.phys_addr   = (uint32_t*)(uintptr_t)info->fb_addr;
    fb.width  = info->width;
    fb.height = info->height;
    fb.pitch  = info->pitch;
    fb.bpp    = info->bpp;

    /* Allocate back-buffer if needed */
    fb_init_backbuffer();

    kinfo("GOP: framebuffer applied %ux%u", fb.width, fb.height);
}

bool uefi_gop_available(void) { return g_available; }
