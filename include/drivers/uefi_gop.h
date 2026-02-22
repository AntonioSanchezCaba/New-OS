/*
 * include/drivers/uefi_gop.h — UEFI GOP framebuffer support
 *
 * Parses the Multiboot2 framebuffer tag to detect a GOP-style linear
 * framebuffer. Falls back to legacy VBE parameters if GOP is unavailable.
 *
 * GOP (Graphics Output Protocol) provides a 32-bit linear framebuffer
 * without requiring VBE calls or real-mode round-trips.
 */
#pragma once
#include <types.h>

typedef enum {
    GOP_PIXEL_RGB  = 0,   /* 0xRRGGBB00 */
    GOP_PIXEL_BGR  = 1,   /* 0x00BBGGRR */
    GOP_PIXEL_BITMASK = 2,
    GOP_PIXEL_BLTONLY = 3,
} gop_pixel_fmt_t;

typedef struct {
    bool          available;
    uint64_t      fb_addr;         /* Physical address of framebuffer */
    uint32_t      width;
    uint32_t      height;
    uint32_t      pitch;           /* Bytes per row */
    uint8_t       bpp;             /* Bits per pixel (should be 32) */
    gop_pixel_fmt_t pixel_format;
    /* Pixel bitmasks (for BITMASK format) */
    uint32_t      red_mask;
    uint32_t      green_mask;
    uint32_t      blue_mask;
} gop_info_t;

/* Parse UEFI/Multiboot2 framebuffer tag and populate gop_info_t.
 * Returns 0 on success, -1 if no GOP framebuffer found. */
int  uefi_gop_init(const void* mb2_info, gop_info_t* out);

/* Apply GOP info to the kernel framebuffer (fb global). */
void uefi_gop_apply(const gop_info_t* info);

/* Return true if running under UEFI with a GOP framebuffer. */
bool uefi_gop_available(void);
