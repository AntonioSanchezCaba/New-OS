/*
 * gui/draw.c - 2D software rendering primitives
 *
 * All operations work on a canvas_t (a rectangular pixel buffer).
 * Colors are 32-bit ARGB (same format as framebuffer back buffer).
 */
#include <gui/draw.h>
#include <drivers/framebuffer.h>
#include <string.h>

/* =========================================================
 * Canvas helpers
 * ========================================================= */

/* Return a canvas wrapping the entire framebuffer back buffer */
canvas_t draw_main_canvas(void)
{
    canvas_t c;
    c.pixels = fb.back_buf;
    c.width  = (int)fb.width;
    c.height = (int)fb.height;
    c.stride = (int)fb.width;
    return c;
}

/* Return a sub-canvas (clipped view into a larger canvas) */
canvas_t draw_sub_canvas(canvas_t* parent, int x, int y, int w, int h)
{
    canvas_t c;
    /* Clip to parent */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > parent->width)  w = parent->width  - x;
    if (y + h > parent->height) h = parent->height - y;
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    c.pixels = parent->pixels + y * parent->stride + x;
    c.width  = w;
    c.height = h;
    c.stride = parent->stride;
    return c;
}

/* =========================================================
 * Pixel-level primitives
 * ========================================================= */

static inline void _put(canvas_t* c, int x, int y, uint32_t color)
{
    if ((unsigned)x < (unsigned)c->width && (unsigned)y < (unsigned)c->height)
        c->pixels[y * c->stride + x] = color;
}

void draw_pixel(canvas_t* c, int x, int y, uint32_t color)
{
    _put(c, x, y, color);
}

/* =========================================================
 * Filled rectangles
 * ========================================================= */

void draw_rect(canvas_t* c, int x, int y, int w, int h, uint32_t color)
{
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w; if (x1 > c->width)  x1 = c->width;
    int y1 = y + h; if (y1 > c->height) y1 = c->height;
    if (x0 >= x1 || y0 >= y1) return;

    int line_w = x1 - x0;
    for (int row = y0; row < y1; row++) {
        uint32_t* dst = c->pixels + row * c->stride + x0;
        for (int i = 0; i < line_w; i++)
            dst[i] = color;
    }
}

/* Filled rectangle with per-pixel alpha blend */
void draw_rect_alpha(canvas_t* c, int x, int y, int w, int h, uint32_t color)
{
    uint8_t a = (uint8_t)(color >> 24);
    if (a == 0xFF) { draw_rect(c, x, y, w, h, color); return; }
    if (a == 0x00) return;

    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w; if (x1 > c->width)  x1 = c->width;
    int y1 = y + h; if (y1 > c->height) y1 = c->height;

    for (int row = y0; row < y1; row++) {
        uint32_t* dst = c->pixels + row * c->stride + x0;
        for (int i = 0; i < x1 - x0; i++)
            dst[i] = fb_blend(dst[i], color);
    }
}

/* Rectangle outline (border only) */
void draw_rect_outline(canvas_t* c, int x, int y, int w, int h,
                       int thickness, uint32_t color)
{
    draw_rect(c, x,             y,             w,         thickness, color);
    draw_rect(c, x,             y + h - thickness, w,     thickness, color);
    draw_rect(c, x,             y + thickness, thickness, h - 2*thickness, color);
    draw_rect(c, x + w - thickness, y + thickness, thickness, h - 2*thickness, color);
}

/* Rounded rectangle (simple: just clip corners with background color) */
void draw_rect_rounded(canvas_t* c, int x, int y, int w, int h,
                       int radius, uint32_t color)
{
    if (radius <= 0) { draw_rect(c, x, y, w, h, color); return; }

    /* Fill three non-corner rectangles */
    draw_rect(c, x + radius, y,          w - 2*radius, h, color);
    draw_rect(c, x,          y + radius, radius,        h - 2*radius, color);
    draw_rect(c, x + w - radius, y + radius, radius,   h - 2*radius, color);

    /* Draw quarter circles at each corner */
    for (int row = 0; row < radius; row++) {
        for (int col = 0; col < radius; col++) {
            int dx = radius - col - 1;
            int dy = radius - row - 1;
            if (dx*dx + dy*dy <= radius*radius) {
                /* Top-left */
                _put(c, x + col, y + row, color);
                /* Top-right */
                _put(c, x + w - 1 - col, y + row, color);
                /* Bottom-left */
                _put(c, x + col, y + h - 1 - row, color);
                /* Bottom-right */
                _put(c, x + w - 1 - col, y + h - 1 - row, color);
            }
        }
    }
}

/* =========================================================
 * Lines
 * ========================================================= */

void draw_hline(canvas_t* c, int x, int y, int len, uint32_t color)
{
    draw_rect(c, x, y, len, 1, color);
}

void draw_vline(canvas_t* c, int x, int y, int len, uint32_t color)
{
    draw_rect(c, x, y, 1, len, color);
}

/* Bresenham's line algorithm */
void draw_line(canvas_t* c, int x0, int y0, int x1, int y1, uint32_t color)
{
    int dx = x1 - x0; if (dx < 0) dx = -dx;
    int dy = y1 - y0; if (dy < 0) dy = -dy;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;

    for (;;) {
        _put(c, x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

/* =========================================================
 * Circles
 * ========================================================= */

/* Midpoint circle (outline) */
void draw_circle(canvas_t* c, int cx, int cy, int r, uint32_t color)
{
    int x = 0, y = r, d = 3 - 2*r;
    while (x <= y) {
        _put(c, cx+x, cy+y, color); _put(c, cx-x, cy+y, color);
        _put(c, cx+x, cy-y, color); _put(c, cx-x, cy-y, color);
        _put(c, cx+y, cy+x, color); _put(c, cx-y, cy+x, color);
        _put(c, cx+y, cy-x, color); _put(c, cx-y, cy-x, color);
        if (d < 0) d += 4*x + 6;
        else { d += 4*(x-y) + 10; y--; }
        x++;
    }
}

void draw_circle_filled(canvas_t* c, int cx, int cy, int r, uint32_t color)
{
    int x = 0, y = r, d = 3 - 2*r;
    while (x <= y) {
        draw_hline(c, cx-x, cy+y, 2*x+1, color);
        draw_hline(c, cx-x, cy-y, 2*x+1, color);
        draw_hline(c, cx-y, cy+x, 2*y+1, color);
        draw_hline(c, cx-y, cy-x, 2*y+1, color);
        if (d < 0) d += 4*x + 6;
        else { d += 4*(x-y) + 10; y--; }
        x++;
    }
}

/* =========================================================
 * Gradients
 * ========================================================= */

/* Vertical gradient from color_top to color_bottom */
void draw_gradient_v(canvas_t* c, int x, int y, int w, int h,
                     uint32_t color_top, uint32_t color_bottom)
{
    if (h <= 0) return;
    uint32_t r0 = (color_top  >> 16) & 0xFF;
    uint32_t g0 = (color_top  >>  8) & 0xFF;
    uint32_t b0 =  color_top         & 0xFF;
    uint32_t r1 = (color_bottom >> 16) & 0xFF;
    uint32_t g1 = (color_bottom >>  8) & 0xFF;
    uint32_t b1 =  color_bottom        & 0xFF;

    for (int row = 0; row < h; row++) {
        uint32_t r = r0 + (r1 - r0) * row / h;
        uint32_t g = g0 + (g1 - g0) * row / h;
        uint32_t b = b0 + (b1 - b0) * row / h;
        draw_rect(c, x, y + row, w, 1, rgb((uint8_t)r,(uint8_t)g,(uint8_t)b));
    }
}

/* =========================================================
 * Blit operations
 * ========================================================= */

/* Copy src pixels into canvas at (x, y). Clips to canvas bounds. */
void draw_blit(canvas_t* c, int x, int y,
               const uint32_t* src, int sw, int sh)
{
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + sw; if (x1 > c->width)  x1 = c->width;
    int y1 = y + sh; if (y1 > c->height) y1 = c->height;

    int src_x_off = x0 - x;
    int src_y_off = y0 - y;

    for (int row = y0; row < y1; row++) {
        uint32_t* dst_row = c->pixels + row * c->stride + x0;
        const uint32_t* src_row = src + (src_y_off + row - y0) * sw + src_x_off;
        int len = x1 - x0;
        memcpy(dst_row, src_row, (size_t)len * sizeof(uint32_t));
    }
}

/* Alpha-blended blit */
void draw_blit_alpha(canvas_t* c, int x, int y,
                     const uint32_t* src, int sw, int sh)
{
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + sw; if (x1 > c->width)  x1 = c->width;
    int y1 = y + sh; if (y1 > c->height) y1 = c->height;

    int src_x_off = x0 - x;
    int src_y_off = y0 - y;

    for (int row = y0; row < y1; row++) {
        uint32_t* dst_row = c->pixels + row * c->stride + x0;
        const uint32_t* src_row = src + (src_y_off + row - y0) * sw + src_x_off;
        int len = x1 - x0;
        for (int i = 0; i < len; i++)
            dst_row[i] = fb_blend(dst_row[i], src_row[i]);
    }
}

/* =========================================================
 * Scroll
 * ========================================================= */

/* Scroll a sub-region of the canvas up by `lines` pixel rows; clear vacated area */
void draw_scroll_up(canvas_t* c, int x, int y, int w, int h,
                    int lines, uint32_t fill_color)
{
    /* Clip region to canvas */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > c->width)  w = c->width  - x;
    if (y + h > c->height) h = c->height - y;
    if (w <= 0 || h <= 0) return;

    if (lines <= 0) return;
    if (lines >= h) {
        draw_rect(c, x, y, w, h, fill_color);
        return;
    }

    /* Move rows up within the sub-region, row by row */
    for (int row = 0; row < h - lines; row++) {
        uint32_t* dst = c->pixels + (y + row) * c->stride + x;
        const uint32_t* src = c->pixels + (y + row + lines) * c->stride + x;
        memmove(dst, src, (size_t)w * sizeof(uint32_t));
    }
    /* Fill vacated rows at the bottom */
    draw_rect(c, x, y + h - lines, w, lines, fill_color);
}
