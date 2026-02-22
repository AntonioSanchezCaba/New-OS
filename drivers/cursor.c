/*
 * drivers/cursor.c — Software cursor sprite compositing
 *
 * Provides 9 cursor types rendered as 1-bit bitmaps with transparency.
 * Each frame:
 *   1. cursor_erase()  — blit saved background back
 *   2. [frame render]
 *   3. cursor_render() — save background, blit sprite
 *
 * No floating point. All coordinates are integers.
 */
#include <drivers/cursor.h>
#include <drivers/framebuffer.h>
#include <drivers/mouse.h>
#include <string.h>
#include <types.h>

/* =========================================================
 * Sprite bitmaps — 1 bit per pixel, row-major
 * Width is padded to 16 bits per row for easy parsing.
 * ========================================================= */

/* Arrow cursor: 12×20 */
#define ARROW_W 12
#define ARROW_H 20
static const uint16_t arrow_bits[ARROW_H] = {
    0b1000000000000000,
    0b1100000000000000,
    0b1110000000000000,
    0b1111000000000000,
    0b1111100000000000,
    0b1111110000000000,
    0b1111111000000000,
    0b1111111100000000,
    0b1111111110000000,
    0b1111111111000000,
    0b1111111111100000,
    0b1111111111000000,
    0b1111110000000000,
    0b1100111000000000,
    0b1000011100000000,
    0b0000001110000000,
    0b0000001110000000,
    0b0000000111000000,
    0b0000000111000000,
    0b0000000000000000,
};
/* Mask — same shape but slightly larger (outline) */
static const uint16_t arrow_mask[ARROW_H] = {
    0b1100000000000000,
    0b1110000000000000,
    0b1111000000000000,
    0b1111100000000000,
    0b1111110000000000,
    0b1111111000000000,
    0b1111111100000000,
    0b1111111110000000,
    0b1111111111000000,
    0b1111111111100000,
    0b1111111111110000,
    0b1111111111110000,
    0b1111110000000000,
    0b1110111100000000,
    0b1100011110000000,
    0b0000001111000000,
    0b0000001111000000,
    0b0000000111100000,
    0b0000000111100000,
    0b0000000000000000,
};

/* Hand cursor: 12×20 */
#define HAND_W 12
#define HAND_H 20
static const uint16_t hand_bits[HAND_H] = {
    0b0011000000000000,
    0b0011000000000000,
    0b0011000000000000,
    0b0011000000000000,
    0b0011011011000000,
    0b0011111111100000,
    0b0111111111110000,
    0b0111111111110000,
    0b0111111111110000,
    0b0111111111110000,
    0b0011111111100000,
    0b0011111111100000,
    0b0001111111000000,
    0b0001111111000000,
    0b0000111110000000,
    0b0000111110000000,
    0b0000011100000000,
    0b0000000000000000,
    0b0000000000000000,
    0b0000000000000000,
};
static const uint16_t hand_mask[HAND_H] = {
    0b0111100000000000,
    0b0111100000000000,
    0b0111100000000000,
    0b0111111111000000,
    0b0111111111100000,
    0b1111111111110000,
    0b1111111111111000,
    0b1111111111111000,
    0b1111111111111000,
    0b1111111111111000,
    0b0111111111111000,
    0b0111111111110000,
    0b0011111111100000,
    0b0011111111100000,
    0b0001111111000000,
    0b0001111111000000,
    0b0000111110000000,
    0b0000000000000000,
    0b0000000000000000,
    0b0000000000000000,
};

/* Text I-beam: 8×20 */
#define TEXT_W 8
#define TEXT_H 20
static const uint16_t text_bits[TEXT_H] = {
    0b1110111000000000,
    0b0001000000000000,
    0b0001000000000000,
    0b0001000000000000,
    0b0001000000000000,
    0b0001000000000000,
    0b0001000000000000,
    0b0001000000000000,
    0b0001000000000000,
    0b0001000000000000,
    0b0001000000000000,
    0b0001000000000000,
    0b0001000000000000,
    0b0001000000000000,
    0b0001000000000000,
    0b0001000000000000,
    0b0001000000000000,
    0b0001000000000000,
    0b0001000000000000,
    0b1110111000000000,
};
static const uint16_t text_mask[TEXT_H] = {
    0b1111111100000000,
    0b1111111100000000,
    0b0011100000000000,
    0b0011100000000000,
    0b0011100000000000,
    0b0011100000000000,
    0b0011100000000000,
    0b0011100000000000,
    0b0011100000000000,
    0b0011100000000000,
    0b0011100000000000,
    0b0011100000000000,
    0b0011100000000000,
    0b0011100000000000,
    0b0011100000000000,
    0b0011100000000000,
    0b0011100000000000,
    0b0011100000000000,
    0b1111111100000000,
    0b1111111100000000,
};

/* Crosshair: 16×16 */
#define CROSS_W 16
#define CROSS_H 16
static const uint16_t cross_bits[CROSS_H] = {
    0b0000001100000000,
    0b0000001100000000,
    0b0000001100000000,
    0b0000001100000000,
    0b0000001100000000,
    0b0000001100000000,
    0b1111110011111100,
    0b1111110011111100,
    0b0000001100000000,
    0b0000001100000000,
    0b0000001100000000,
    0b0000001100000000,
    0b0000001100000000,
    0b0000001100000000,
    0b0000000000000000,
    0b0000000000000000,
};
static const uint16_t cross_mask[CROSS_H] = {
    0b0000011110000000,
    0b0000011110000000,
    0b0000011110000000,
    0b0000011110000000,
    0b0000011110000000,
    0b1111111111111100,
    0b1111111111111100,
    0b1111111111111100,
    0b1111111111111100,
    0b0000011110000000,
    0b0000011110000000,
    0b0000011110000000,
    0b0000011110000000,
    0b0000011110000000,
    0b0000000000000000,
    0b0000000000000000,
};

/* Wait/busy (simple block): 12×12 */
#define WAIT_W 12
#define WAIT_H 12
static const uint16_t wait_bits[WAIT_H] = {
    0b0111111110000000,
    0b1000000001000000,
    0b1000000001000000,
    0b0100000010000000,
    0b0010000100000000,
    0b0001001000000000,
    0b0001001000000000,
    0b0010000100000000,
    0b0100000010000000,
    0b1000000001000000,
    0b1000000001000000,
    0b0111111110000000,
};
static const uint16_t wait_mask[WAIT_H] = {
    0b0111111110000000,
    0b1111111111000000,
    0b1111111111000000,
    0b0111111110000000,
    0b0011111100000000,
    0b0001111000000000,
    0b0001111000000000,
    0b0011111100000000,
    0b0111111110000000,
    0b1111111111000000,
    0b1111111111000000,
    0b0111111110000000,
};

/* Resize H: ←→ 16×10 */
#define RESZH_W 16
#define RESZH_H 10
static const uint16_t reszh_bits[RESZH_H] = {
    0b0000100000010000,
    0b0001100000110000,
    0b0011111111110000,
    0b0111111111111000,
    0b0011111111110000,
    0b0001100000110000,
    0b0000100000010000,
    0b0000000000000000,
    0b0000000000000000,
    0b0000000000000000,
};
static const uint16_t reszh_mask[RESZH_H] = {
    0b0001110000111000,
    0b0011110001111000,
    0b0111111111111100,
    0b1111111111111110,
    0b0111111111111100,
    0b0011110001111000,
    0b0001110000111000,
    0b0000000000000000,
    0b0000000000000000,
    0b0000000000000000,
};

/* Resize V: ↕ — 10×16 */
#define RESZV_W 10
#define RESZV_H 16
static const uint16_t reszv_bits[RESZV_H] = {
    0b0000100000000000,
    0b0001110000000000,
    0b0011111000000000,
    0b0101010000000000,
    0b0001110000000000,
    0b0001110000000000,
    0b0001110000000000,
    0b0001110000000000,
    0b0001110000000000,
    0b0001110000000000,
    0b0001110000000000,
    0b0101010000000000,
    0b0011111000000000,
    0b0001110000000000,
    0b0000100000000000,
    0b0000000000000000,
};
static const uint16_t reszv_mask[RESZV_H] = {
    0b0001110000000000,
    0b0011111000000000,
    0b0111111100000000,
    0b1111111110000000,
    0b0011111000000000,
    0b0011111000000000,
    0b0011111000000000,
    0b0011111000000000,
    0b0011111000000000,
    0b0011111000000000,
    0b1111111110000000,
    0b0111111100000000,
    0b0011111000000000,
    0b0001110000000000,
    0b0000000000000000,
    0b0000000000000000,
};

/* Diagonal resize: ↘ 12×12 */
#define RESZD_W 12
#define RESZD_H 12
static const uint16_t reszd_bits[RESZD_H] = {
    0b1111110000000000,
    0b1111000000000000,
    0b1110000000000000,
    0b1101000000000000,
    0b1000100000000000,
    0b0000010000000000,
    0b0000001000000000,
    0b0000000100110000,
    0b0000000011110000,
    0b0000000001110000,
    0b0000000011110000,
    0b0000000111110000,
};
static const uint16_t reszd_mask[RESZD_H] = {
    0b1111111000000000,
    0b1111110000000000,
    0b1111100000000000,
    0b1111100000000000,
    0b1101110000000000,
    0b0001111000000000,
    0b0000011100000000,
    0b0000001111110000,
    0b0000000111110000,
    0b0000000111110000,
    0b0000000111110000,
    0b0000001111110000,
};

/* =========================================================
 * Sprite descriptor table
 * ========================================================= */
typedef struct {
    const uint16_t* bits;
    const uint16_t* mask;
    int             w, h;
    int             hot_x, hot_y;  /* Hotspot (tip) within sprite */
} sprite_def_t;

static const sprite_def_t g_sprites[CURSOR_COUNT] = {
    [CURSOR_ARROW]    = { arrow_bits,  arrow_mask,  ARROW_W,  ARROW_H,  0, 0 },
    [CURSOR_HAND]     = { hand_bits,   hand_mask,   HAND_W,   HAND_H,   4, 0 },
    [CURSOR_TEXT]     = { text_bits,   text_mask,   TEXT_W,   TEXT_H,   4, 10},
    [CURSOR_WAIT]     = { wait_bits,   wait_mask,   WAIT_W,   WAIT_H,   5, 5 },
    [CURSOR_RESIZE_H] = { reszh_bits,  reszh_mask,  RESZH_W,  RESZH_H,  7, 4 },
    [CURSOR_RESIZE_V] = { reszv_bits,  reszv_mask,  RESZV_W,  RESZV_H,  5, 8 },
    [CURSOR_RESIZE_D] = { reszd_bits,  reszd_mask,  RESZD_W,  RESZD_H,  0, 0 },
    [CURSOR_CROSS]    = { cross_bits,  cross_mask,  CROSS_W,  CROSS_H,  7, 7 },
    [CURSOR_NONE]     = { NULL, NULL, 0, 0, 0, 0 },
};

/* =========================================================
 * Cursor state
 * ========================================================= */
static struct {
    int           x, y;
    cursor_type_t type;
    bool          visible;

    /* Saved background under sprite */
    uint32_t      bg[CURSOR_MAX_W * CURSOR_MAX_H];
    int           bg_x, bg_y, bg_w, bg_h;
    bool          bg_saved;
} g_cur = {
    .x = 400, .y = 300,
    .type = CURSOR_ARROW,
    .visible = true,
    .bg_saved = false,
};

/* =========================================================
 * Public API
 * ========================================================= */
void cursor_init(void) { g_cur.bg_saved = false; }

void cursor_show(void) { g_cur.visible = true; }
void cursor_hide(void) { g_cur.visible = false; }

void cursor_set_type(cursor_type_t type)
{
    if (type >= CURSOR_COUNT) type = CURSOR_ARROW;
    g_cur.type = type;
}

void cursor_move(int x, int y)
{
    g_cur.x = x;
    g_cur.y = y;
}

int           cursor_x(void)          { return g_cur.x; }
int           cursor_y(void)          { return g_cur.y; }
cursor_type_t cursor_get_type(void)   { return g_cur.type; }
bool          cursor_is_visible(void) { return g_cur.visible; }

/* =========================================================
 * cursor_erase() — restore saved background
 * ========================================================= */
void cursor_erase(void)
{
    if (!g_cur.bg_saved || !fb.phys_addr) return;

    uint32_t* fb_ptr = (uint32_t*)fb.phys_addr;
    int pitch = (int)(fb.pitch / 4);

    for (int row = 0; row < g_cur.bg_h; row++) {
        int fy = g_cur.bg_y + row;
        if (fy < 0 || fy >= (int)fb.height) continue;
        for (int col = 0; col < g_cur.bg_w; col++) {
            int fx = g_cur.bg_x + col;
            if (fx < 0 || fx >= (int)fb.width) continue;
            fb_ptr[fy * pitch + fx] = g_cur.bg[row * g_cur.bg_w + col];
        }
    }
    g_cur.bg_saved = false;
}

/* =========================================================
 * cursor_render() — save background then blit sprite
 * ========================================================= */
void cursor_render(void)
{
    if (!g_cur.visible || !fb.phys_addr) return;
    if (g_cur.type == CURSOR_NONE) return;

    const sprite_def_t* sp = &g_sprites[g_cur.type];
    if (!sp->bits) return;

    /* Top-left corner of sprite on screen */
    int sx = g_cur.x - sp->hot_x;
    int sy = g_cur.y - sp->hot_y;
    int sw = sp->w;
    int sh = sp->h;

    /* Clamp */
    int draw_x = sx, draw_y = sy;
    int draw_w = sw, draw_h = sh;
    if (draw_x < 0) { draw_w += draw_x; draw_x = 0; }
    if (draw_y < 0) { draw_h += draw_y; draw_y = 0; }
    if (draw_x + draw_w > (int)fb.width)  draw_w = (int)fb.width  - draw_x;
    if (draw_y + draw_h > (int)fb.height) draw_h = (int)fb.height - draw_y;
    if (draw_w <= 0 || draw_h <= 0) return;

    /* Save background */
    g_cur.bg_x = draw_x;
    g_cur.bg_y = draw_y;
    g_cur.bg_w = draw_w;
    g_cur.bg_h = draw_h;
    g_cur.bg_saved = true;

    uint32_t* fb_ptr = (uint32_t*)fb.phys_addr;
    int pitch = (int)(fb.pitch / 4);

    for (int row = 0; row < draw_h; row++) {
        int fy = draw_y + row;
        int sr = sy < 0 ? row - sy : row;    /* Row in sprite bitmap */
        if (sr >= sh) break;

        uint16_t bits_row = sp->bits[sr];
        uint16_t mask_row = sp->mask[sr];

        for (int col = 0; col < draw_w; col++) {
            int fx  = draw_x + col;
            int sc  = sx < 0 ? col - sx : col;  /* Col in sprite */
            if (sc >= sw) break;

            int offset = fy * pitch + fx;
            /* Save background pixel */
            g_cur.bg[row * draw_w + col] = fb_ptr[offset];

            /* Test mask bit (MSB of 16-bit word) */
            uint16_t mbit = (uint16_t)(mask_row >> (15 - sc));
            if (!(mbit & 1)) continue;   /* Transparent pixel */

            /* Foreground: white sprite with black outline */
            uint16_t fbit = (uint16_t)(bits_row >> (15 - sc));
            if (fbit & 1) {
                fb_ptr[offset] = 0xFFFFFFFF;  /* White fill */
            } else {
                fb_ptr[offset] = 0xFF000000;  /* Black outline */
            }
        }
    }
}
