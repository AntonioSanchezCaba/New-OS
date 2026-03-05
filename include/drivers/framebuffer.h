/*
 * include/drivers/framebuffer.h - Linear framebuffer driver
 *
 * Supports VESA/GOP linear framebuffers provided by Multiboot2.
 * Implements double-buffering for tear-free rendering.
 */
#ifndef DRIVERS_FRAMEBUFFER_H
#define DRIVERS_FRAMEBUFFER_H

#include <types.h>
#include <multiboot2.h>

/* Damage rectangle - marks a dirty screen region for partial flip */
typedef struct {
    int x, y, w, h;
} fb_rect_t;

#define FB_MAX_DAMAGE  64   /* Max dirty rectangles per frame */

/* Framebuffer state */
typedef struct {
    uint32_t* phys_addr;    /* Physical framebuffer address */
    uint32_t* back_buf;     /* Back buffer (in kernel heap) */
    uint32_t  width;
    uint32_t  height;
    uint32_t  pitch;        /* Bytes per scanline */
    uint8_t   bpp;          /* Bits per pixel */
    bool      initialized;

    /* Damage tracking for partial flips */
    fb_rect_t damage[FB_MAX_DAMAGE];
    int       damage_count;
    bool      full_damage;  /* If true, flip everything (first frame etc.) */

    /* Frame statistics */
    uint64_t  frame_count;
    uint64_t  last_flip_tick;
} framebuffer_t;

extern framebuffer_t fb;

/* Compile-time constant RGB macro (usable in static initializers) */
#define RGB(r,g,b)  (0xFF000000u | (uint32_t)(r) << 16 | (uint32_t)(g) << 8 | (uint32_t)(b))

/* 32-bit ARGB color constructors */
static inline uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return (0xFF000000u) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static inline uint32_t rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

/* Named colors */
#define COLOR_BLACK       rgb(0x00, 0x00, 0x00)
#define COLOR_WHITE       rgb(0xFF, 0xFF, 0xFF)
#define COLOR_RED         rgb(0xE0, 0x40, 0x40)
#define COLOR_GREEN       rgb(0x40, 0xC0, 0x40)
#define COLOR_BLUE        rgb(0x40, 0x80, 0xE0)
#define COLOR_CYAN        rgb(0x40, 0xC0, 0xE0)
#define COLOR_MAGENTA     rgb(0xC0, 0x40, 0xC0)
#define COLOR_YELLOW      rgb(0xE0, 0xC0, 0x40)
#define COLOR_DARK_GREY   rgb(0x30, 0x30, 0x30)
#define COLOR_LIGHT_GREY  rgb(0xA0, 0xA0, 0xA0)
#define COLOR_MID_GREY    rgb(0x60, 0x60, 0x60)

/* Desktop theme colors */
#define COLOR_DESKTOP_BG  rgb(0x1E, 0x2A, 0x3A)
#define COLOR_TASKBAR_BG  rgb(0x10, 0x18, 0x28)
#define COLOR_WIN_TITLE   rgb(0x2A, 0x52, 0x8A)
#define COLOR_WIN_BG      rgb(0xF0, 0xF0, 0xF0)
#define COLOR_WIN_BORDER  rgb(0x20, 0x40, 0x70)
#define COLOR_BTN_NORMAL  rgb(0x50, 0x90, 0xD0)
#define COLOR_BTN_HOVER   rgb(0x60, 0xA0, 0xE0)
#define COLOR_BTN_CLOSE   rgb(0xC0, 0x30, 0x30)
#define COLOR_TEXT_DARK   rgb(0x10, 0x10, 0x10)
#define COLOR_TEXT_LIGHT  rgb(0xF0, 0xF0, 0xF0)
#define COLOR_HIGHLIGHT   rgb(0x30, 0x70, 0xC0)
#define COLOR_ACCENT      rgb(0x00, 0x7A, 0xCC)

/* Framebuffer API */
void    fb_raw_setup(uintptr_t phys, uint32_t w, uint32_t h, uint32_t pitch);
void    fb_paint_panic(uint32_t colour);
void    fb_init(struct multiboot2_tag_framebuffer* fb_tag);
void    fb_flip(void);                /* Full back buffer -> physical fb */
void    fb_flip_damage(void);         /* Flip only dirty damage regions */
void    fb_damage(int x, int y, int w, int h); /* Mark region dirty */
void    fb_damage_full(void);         /* Mark entire screen dirty */
void    fb_damage_clear(void);        /* Reset damage list */
void    fb_clear(uint32_t color);     /* Clear back buffer */
void    fb_put_pixel(int x, int y, uint32_t color);
uint32_t fb_get_pixel(int x, int y);
void    fb_blit_region(int dst_x, int dst_y,
                        const uint32_t* src, int src_w, int src_h, int src_pitch);
void    fb_blit_alpha(int dst_x, int dst_y,
                       const uint32_t* src, int src_w, int src_h, int src_pitch);
bool    fb_ready(void);
void    fb_init_backbuffer(void);
uint64_t fb_frame_count(void);

/* Alpha-blend two colors (src over dst, 8-bit alpha) */
static inline uint32_t fb_blend(uint32_t dst, uint32_t src)
{
    uint8_t a = (src >> 24) & 0xFF;
    if (a == 0xFF) return src;
    if (a == 0x00) return dst;

    uint32_t sr = (src >> 16) & 0xFF;
    uint32_t sg = (src >>  8) & 0xFF;
    uint32_t sb =  src        & 0xFF;
    uint32_t dr = (dst >> 16) & 0xFF;
    uint32_t dg = (dst >>  8) & 0xFF;
    uint32_t db =  dst        & 0xFF;

    uint32_t r = (sr * a + dr * (255 - a)) / 255;
    uint32_t g = (sg * a + dg * (255 - a)) / 255;
    uint32_t b = (sb * a + db * (255 - a)) / 255;
    return rgb((uint8_t)r, (uint8_t)g, (uint8_t)b);
}

#endif /* DRIVERS_FRAMEBUFFER_H */
