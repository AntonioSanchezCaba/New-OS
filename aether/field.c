/*
 * aether/field.c — Field rendering and layout
 *
 * The Field is the spatial canvas. It renders:
 *   1. Dynamic background (deep-space with drifting particles)
 *   2. Surface shadows
 *   3. All visible surfaces (blit with scale + alpha)
 *   4. Active surface edge glow
 *   5. Navigation dots (bottom center)
 *   6. Overview mode grid
 */
#include <aether/field.h>
#include <aether/are.h>
#include <aether/context.h>
#include <gui/draw.h>
#include <drivers/timer.h>
#include <memory.h>
#include <string.h>
#include <kernel.h>

/* =========================================================
 * Field state
 * ========================================================= */
static field_state_t g_field;
static uint32_t g_screen_w, g_screen_h;

/* =========================================================
 * Background palette
 * ========================================================= */
#define BG_TOP    ACOLOR(0x09, 0x0D, 0x1A, 0xFF)   /* Deep space */
#define BG_MID    ACOLOR(0x0D, 0x13, 0x26, 0xFF)
#define BG_BOT    ACOLOR(0x07, 0x0A, 0x14, 0xFF)

/* =========================================================
 * Init
 * ========================================================= */
void field_init(uint32_t sw, uint32_t sh)
{
    g_screen_w = sw;
    g_screen_h = sh;
    memset(&g_field, 0, sizeof(g_field));

    /* Seed particles deterministically */
    uint32_t seed = 0xAE74F012;
    for (int i = 0; i < 120; i++) {
        seed = seed * 1664525u + 1013904223u;
        g_field.particles[i].x = (int16_t)(seed % sw);
        seed = seed * 1664525u + 1013904223u;
        g_field.particles[i].y = (int16_t)(seed % sh);
        seed = seed * 1664525u + 1013904223u;
        g_field.particles[i].speed      = (uint8_t)(1 + seed % 3);
        g_field.particles[i].brightness = (uint8_t)(60 + seed % 100);
    }
    kinfo("ARE: field initialized (%ux%u)", sw, sh);
}

/* =========================================================
 * Slot geometry
 * ========================================================= */
slot_geom_t field_slot_geom(int slot, uint32_t sw, uint32_t sh,
                              uint32_t scr_w, uint32_t scr_h)
{
    slot_geom_t g;

    /* Usable area */
    int uw = (int)scr_w - 2 * FIELD_SURF_MARGIN_X;
    int uh = (int)scr_h - FIELD_SURF_MARGIN_TOP - FIELD_SURF_MARGIN_BOT;

    /* Scale factor for each slot (256 = 100%) */
    int scale, alpha;
    if (slot == 0) {
        scale = FIELD_SCALE_ACTIVE;
        alpha = FIELD_ALPHA_ACTIVE;
    } else if (slot == 1 || slot == -1) {
        scale = FIELD_SCALE_ADJACENT;
        alpha = FIELD_ALPHA_ADJACENT;
    } else if (slot == 2 || slot == -2) {
        scale = FIELD_SCALE_FAR;
        alpha = FIELD_ALPHA_FAR;
    } else {
        g.pos   = vec2(-10000, -10000);
        g.w     = 0;
        g.h     = 0;
        g.alpha = 0;
        g.slot  = slot;
        return g;
    }

    /* Scaled dimensions, capped to usable area */
    int dw = ((int)sw * scale) / 256;
    int dh = ((int)sh * scale) / 256;
    if (dw > uw) { dh = dh * uw / dw; dw = uw; }
    if (dh > uh) { dw = dw * uh / dh; dh = uh; }

    /* Horizontal offset: each slot is spaced by screen_w */
    int center_x = (int)scr_w / 2;
    int center_y = FIELD_SURF_MARGIN_TOP + uh / 2;

    int pos_x = center_x + slot * (int)scr_w - dw / 2;
    int pos_y = center_y - dh / 2;

    g.pos   = vec2(pos_x, pos_y);
    g.w     = dw;
    g.h     = dh;
    g.alpha = (uint8_t)alpha;
    g.slot  = slot;
    return g;
}

/* =========================================================
 * Background tick (animate particles)
 * ========================================================= */
void field_tick(void)
{
    g_field.tick++;
    /* Drift particles slowly downward */
    for (int i = 0; i < 120; i++) {
        if ((g_field.tick % g_field.particles[i].speed) == 0) {
            g_field.particles[i].y++;
            if (g_field.particles[i].y >= (int16_t)g_screen_h)
                g_field.particles[i].y = 0;
        }
    }
}

/* =========================================================
 * Draw background
 * ========================================================= */
void field_draw_background(canvas_t* c)
{
    /* Vertical gradient: deep space */
    uint32_t top = ACOLOR(0x09,0x0D,0x1A,0xFF);
    uint32_t bot = ACOLOR(0x07,0x0A,0x14,0xFF);
    for (int y = 0; y < (int)c->height; y++) {
        int t = (y * 255) / (int)c->height;
        uint8_t r = (uint8_t)((ACOLOR_R(top)*(255-t) + ACOLOR_R(bot)*t) / 255);
        uint8_t g2= (uint8_t)((ACOLOR_G(top)*(255-t) + ACOLOR_G(bot)*t) / 255);
        uint8_t b = (uint8_t)((ACOLOR_B(top)*(255-t) + ACOLOR_B(bot)*t) / 255);
        uint32_t row_color = (0xFF<<24)|(r<<16)|(g2<<8)|b;
        for (int x = 0; x < (int)c->width; x++)
            c->pixels[y * c->width + x] = row_color;
    }

    /* Draw particles */
    for (int i = 0; i < 120; i++) {
        int px = g_field.particles[i].x;
        int py = g_field.particles[i].y;
        uint8_t br = g_field.particles[i].brightness;
        if (px < 0 || px >= (int)c->width) continue;
        if (py < 0 || py >= (int)c->height) continue;
        c->pixels[py * c->width + px] = (0xFF<<24)|(br<<16)|(br<<8)|br;
    }

    /* Subtle horizontal accent line near top */
    uint32_t accent = ACOLOR(0x1A, 0x2D, 0x5A, 0x40);
    for (int x = 0; x < (int)c->width; x++)
        c->pixels[FIELD_SURF_MARGIN_TOP / 2 * c->width + x] = accent;
}

/* =========================================================
 * Compose all surfaces
 * ========================================================= */
void field_compose(canvas_t* c)
{
    /* Draw from farthest to nearest (back to front) */
    /* First pass: non-active surfaces farthest away */
    for (int pass = 0; pass < 3; pass++) {
        /* pass 0: |slot| >= 2, pass 1: |slot| == 1, pass 2: slot == 0 */
        for (int i = 0; i < g_ctx.field_count; i++) {
            surface_t* s = surface_get(g_ctx.field[i]);
            if (!s || s->cur_alpha == 0 || s->cur_w <= 0 || s->cur_h <= 0)
                continue;

            int slot = i - g_ctx.active_idx;
            if (g_ctx.transitioning)
                ; /* use raw slot during transition */

            bool is_pass = false;
            if (pass == 0 && (slot <= -2 || slot >= 2)) is_pass = true;
            if (pass == 1 && (slot == -1 || slot == 1))  is_pass = true;
            if (pass == 2 && slot == 0)                   is_pass = true;
            if (!is_pass) continue;

            /* Shadow behind surface */
            if (s->cur_alpha > 60) {
                int sx = s->cur_pos.x + 6;
                int sy = s->cur_pos.y + 8;
                uint8_t shad_a = (uint8_t)((int)s->cur_alpha * 40 / 255);
                for (int row = 0; row < s->cur_h && sy+row < (int)c->height; row++) {
                    for (int col = 0; col < s->cur_w && sx+col < (int)c->width; col++) {
                        int dx = sx+col, dy = sy+row;
                        if (dx < 0 || dy < 0) continue;
                        uint32_t* p = &c->pixels[dy * c->width + dx];
                        uint32_t bg = *p;
                        uint8_t br = (uint8_t)((ACOLOR_R(bg)*(255-shad_a))/255);
                        uint8_t bg2= (uint8_t)((ACOLOR_G(bg)*(255-shad_a))/255);
                        uint8_t bb = (uint8_t)((ACOLOR_B(bg)*(255-shad_a))/255);
                        *p = (0xFF<<24)|(br<<16)|(bg2<<8)|bb;
                    }
                }
            }

            /* Blit surface */
            are_blit_surface(c->pixels, (int)c->width, (int)c->height,
                              s->buf, (int)s->buf_w, (int)s->buf_h,
                              s->cur_pos.x, s->cur_pos.y,
                              s->cur_w, s->cur_h, s->cur_alpha);

            /* Active surface: draw thin glow border */
            if (slot == 0 && !g_ctx.overview) {
                uint32_t glow = ACOLOR(0x60, 0xA0, 0xFF, 0x80);
                draw_rect_outline(c, s->cur_pos.x, s->cur_pos.y,
                                   s->cur_w, s->cur_h, 1, glow);
            }
        }
    }

    /* Overlay surfaces (on top of everything) */
    if (context_overlay_visible()) {
        for (int i = 0; i < g_ctx.overlay_count; i++) {
            surface_t* s = surface_get(g_ctx.overlays[i]);
            if (!s || s->cur_alpha == 0) continue;
            are_blit_surface(c->pixels, (int)c->width, (int)c->height,
                              s->buf, (int)s->buf_w, (int)s->buf_h,
                              s->cur_pos.x, s->cur_pos.y,
                              s->cur_w, s->cur_h, s->cur_alpha);
        }
    }
}

/* =========================================================
 * Navigation dots
 * ========================================================= */
void field_draw_nav_dots(canvas_t* c)
{
    int n = g_ctx.field_count;
    if (n <= 1) return;

    int dot_r   = 4;
    int dot_gap = 14;
    int total_w = n * dot_gap - (dot_gap - dot_r * 2);
    int ox      = ((int)c->width - total_w) / 2;
    int oy      = (int)c->height - 18;

    for (int i = 0; i < n; i++) {
        int cx = ox + i * dot_gap + dot_r;
        bool active = (i == g_ctx.active_idx);
        uint32_t col = active
            ? ACOLOR(0xA0, 0xC8, 0xFF, 0xFF)
            : ACOLOR(0x40, 0x50, 0x70, 0xFF);
        draw_circle_filled(c, cx, oy, dot_r, col);
        if (active)
            draw_circle_filled(c, cx, oy, dot_r - 2,
                                ACOLOR(0xD0, 0xE8, 0xFF, 0xFF));
    }
}

/* =========================================================
 * Overview
 * ========================================================= */
void field_draw_overview(canvas_t* c)
{
    /* Overview is handled by context_tick computing grid positions.
     * Here we add labels below each surface thumbnail. */
    int cols = FIELD_OVERVIEW_COLS;
    int cell_w = (int)c->width / cols;

    for (int i = 0; i < g_ctx.field_count; i++) {
        surface_t* s = surface_get(g_ctx.field[i]);
        if (!s || !s->cur_alpha) continue;

        int col = i % cols;
        int row = i / cols;
        int lx  = col * cell_w + 8;
        int ly  = s->cur_pos.y + s->cur_h + 4;
        (void)row;

        uint32_t tcol = (g_ctx.overview_hover == i)
            ? ACOLOR(0xFF,0xFF,0xFF,0xFF)
            : ACOLOR(0x90,0xA8,0xC8,0xFF);

        draw_string(c, lx, ly, s->title, tcol, ACOLOR(0,0,0,0));
    }
}

/* =========================================================
 * Overview hit-test
 * ========================================================= */
int field_overview_hit(int mx, int my, uint32_t sw, uint32_t sh)
{
    int cols = FIELD_OVERVIEW_COLS;
    int cell_w = (int)sw / cols;
    int cell_h = (int)sh / cols;

    for (int i = 0; i < g_ctx.field_count; i++) {
        int col = i % cols;
        int row = i / cols;
        rect_t r = rect(col*cell_w+8, row*cell_h+8,
                        cell_w-16, cell_h-16);
        if (rect_contains(r, mx, my)) return i;
    }
    return -1;
}
