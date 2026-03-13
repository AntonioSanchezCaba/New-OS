/*
 * include/gui/draw.h - Software 2D drawing primitives
 *
 * All drawing targets the framebuffer back buffer.
 * Coordinates are in pixels, origin top-left.
 */
#ifndef GUI_DRAW_H
#define GUI_DRAW_H

#include <types.h>
#include <drivers/framebuffer.h>

/* A render target (window buffer or main back buffer) */
typedef struct {
    uint32_t* pixels;
    int       width;
    int       height;
    int       stride;    /* Pixels per row (may differ from width for sub-surfaces) */
} canvas_t;

/* Get a canvas pointing at the main back buffer */
canvas_t draw_main_canvas(void);

/* Get a clipped sub-canvas (view into a region of another canvas) */
canvas_t draw_sub_canvas(canvas_t* parent, int x, int y, int w, int h);

/* ---- Primitive drawing functions ---- */

/* Single pixel */
void draw_pixel(canvas_t* c, int x, int y, uint32_t color);

/* Filled rectangle */
void draw_rect(canvas_t* c, int x, int y, int w, int h, uint32_t color);

/* Filled rectangle with per-pixel alpha blend (uses color's alpha channel) */
void draw_rect_alpha(canvas_t* c, int x, int y, int w, int h, uint32_t color);

/* Clear entire canvas to a solid color */
static inline void draw_clear(canvas_t* c, uint32_t color) {
    draw_rect(c, 0, 0, c->width, c->height, color);
}

/* Outlined rectangle */
void draw_rect_outline(canvas_t* c, int x, int y, int w, int h,
                        int border, uint32_t color);

/* Horizontal / vertical line */
void draw_hline(canvas_t* c, int x, int y, int len, uint32_t color);
void draw_vline(canvas_t* c, int x, int y, int len, uint32_t color);

/* Bresenham line */
void draw_line(canvas_t* c, int x0, int y0, int x1, int y1, uint32_t color);

/* Circle (outline) */
void draw_circle(canvas_t* c, int cx, int cy, int r, uint32_t color);

/* Filled circle */
void draw_circle_filled(canvas_t* c, int cx, int cy, int r, uint32_t color);

/* Rounded rectangle (filled) */
void draw_rect_rounded(canvas_t* c, int x, int y, int w, int h,
                        int radius, uint32_t color);

/* Rounded rectangle outline (border only) */
void draw_rect_rounded_outline(canvas_t* c, int x, int y, int w, int h,
                                int radius, int thickness, uint32_t color);

/* Gradient fill (top-to-bottom) */
void draw_gradient_v(canvas_t* c, int x, int y, int w, int h,
                      uint32_t top_color, uint32_t bottom_color);

/* Blit a pixel buffer (source must be width*height uint32_t) */
void draw_blit(canvas_t* dst, int dx, int dy,
                const uint32_t* src, int sw, int sh);

/* Blit with per-pixel alpha blending */
void draw_blit_alpha(canvas_t* dst, int dx, int dy,
                      const uint32_t* src, int sw, int sh);

/* Scrolling: shift contents of a rect up by @lines pixels */
void draw_scroll_up(canvas_t* c, int x, int y, int w, int h, int lines,
                     uint32_t fill_color);

/* ---- Text rendering (uses the built-in 8x16 font) ---- */

#define FONT_W 8
#define FONT_H 16

/* Draw a single character at pixel position */
void draw_char(canvas_t* c, int x, int y, char ch,
                uint32_t fg, uint32_t bg);

/* Draw a null-terminated string (no wrapping) */
void draw_string(canvas_t* c, int x, int y, const char* str,
                  uint32_t fg, uint32_t bg);

/* Draw formatted string (printf style) */
void draw_printf(canvas_t* c, int x, int y,
                  uint32_t fg, uint32_t bg,
                  const char* fmt, ...);

/* String dimensions */
int draw_string_width(const char* str);
int draw_string_height(void);

/* Draw string centered in a rect */
void draw_string_centered(canvas_t* c, int x, int y, int w, int h,
                            const char* str, uint32_t fg, uint32_t bg);

#endif /* GUI_DRAW_H */
