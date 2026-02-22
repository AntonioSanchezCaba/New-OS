/*
 * include/gui/wallpaper.h — Desktop wallpaper engine
 *
 * Supports BMP (24/32-bit uncompressed) and PPM (P6 binary) image formats.
 * Falls back to a gradient if no image is loaded.
 */
#pragma once
#include <gui/draw.h>

typedef enum {
    WALLPAPER_SOLID    = 0,   /* Single color fill          */
    WALLPAPER_GRADIENT = 1,   /* Vertical gradient          */
    WALLPAPER_IMAGE    = 2,   /* Loaded from file           */
    WALLPAPER_TILED    = 3,   /* Tiled pattern              */
} wallpaper_mode_t;

typedef enum {
    WP_SCALE_FILL    = 0,   /* Scale to fill (may crop)   */
    WP_SCALE_FIT     = 1,   /* Scale to fit (letterbox)   */
    WP_SCALE_CENTER  = 2,   /* Centered, no scale         */
    WP_SCALE_TILE    = 3,   /* Tiled 1:1                  */
} wallpaper_scale_t;

/* Initialize with default gradient */
void wallpaper_init(void);

/* Set solid color */
void wallpaper_set_color(uint32_t color);

/* Set vertical gradient */
void wallpaper_set_gradient(uint32_t top, uint32_t bottom);

/* Load image from VFS path. Returns 0 on success. */
int  wallpaper_load(const char* path, wallpaper_scale_t scale);

/* Draw the wallpaper onto a canvas (covers 0,0 to canvas bounds) */
void wallpaper_draw(canvas_t* c);

/* Query current mode */
wallpaper_mode_t wallpaper_get_mode(void);
