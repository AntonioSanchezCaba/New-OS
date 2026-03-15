/*
 * services/login.c - AetherOS Sci-Fi Login Screen
 *
 * Professional dark-themed login with:
 *   - Large clock display on the left
 *   - Date and system info below clock
 *   - Frosted glass authentication card on the right
 *   - 3D sphere avatar with glossy highlight
 *   - Single password field with shield icon
 *   - Bottom action bar (Shutdown / Restart / Sleep)
 *   - Subtle grid pattern and decorative elements
 */
#include <services/login.h>
#include <kernel/version.h>
#include <gui/draw.h>
#include <gui/theme.h>
#include <drivers/framebuffer.h>
#include <drivers/keyboard.h>
#include <drivers/timer.h>
#include <drivers/mouse.h>
#include <drivers/cursor.h>
#include <scheduler.h>
#include <kernel/users.h>
#include <string.h>
#include <memory.h>

char login_username[64] = "user";

/* ── Extern font data for scaled rendering ─────────────────────────────── */
extern const uint8_t font_data[256][16];

/* ── Layout constants ─────────────────────────────────────────────────── */
#define CARD_W          400
#define CARD_H          340
#define CARD_RADIUS     16
#define FIELD_W         300
#define FIELD_H         40
#define FIELD_RADIUS    8
#define FIELD_PAD       40       /* left padding for icon area */
#define BTN_W           300
#define BTN_H           44
#define BTN_RADIUS      8
#define AVATAR_R        48
#define STATUSBAR_H     0        /* no bottom bar; we use inline footer */
#define GRID_SPACING    40       /* background grid line spacing */

/* ── Color palette (dark sci-fi) ──────────────────────────────────────── */
#define C_BG_TOP        rgb(0x0B, 0x14, 0x26)
#define C_BG_BOT        rgb(0x06, 0x0C, 0x1A)
#define C_GRID          rgba(0x20, 0x40, 0x70, 0x18)
#define C_GLOW          rgba(0x20, 0x50, 0x90, 0x00)  /* base for glow calc */
#define C_CLOCK         rgba(0x70, 0xD0, 0xF8, 0xFF)
#define C_CLOCK_DIM     rgba(0x40, 0x80, 0xB0, 0xFF)
#define C_DATE          rgba(0x60, 0xB0, 0xD8, 0xFF)
#define C_INFO          rgba(0x50, 0x80, 0xA8, 0xFF)
#define C_INFO_DIM      rgba(0x38, 0x60, 0x80, 0xFF)
#define C_CARD_BG       rgba(0x18, 0x28, 0x40, 0xCC)   /* semi-transparent */
#define C_CARD_BORDER   rgba(0x40, 0x70, 0xA0, 0x60)
#define C_CARD_SHINE    rgba(0x60, 0x90, 0xC0, 0x20)   /* top edge highlight */
#define C_AVATAR_DARK   rgb(0x10, 0x20, 0x38)
#define C_AVATAR_MID    rgb(0x28, 0x50, 0x78)
#define C_AVATAR_HI     rgb(0x80, 0xC0, 0xE0)
#define C_AVATAR_RING   rgba(0x40, 0x80, 0xC0, 0x80)
#define C_NAME          rgba(0xD0, 0xE8, 0xF8, 0xFF)
#define C_SUBTEXT       rgba(0x60, 0x88, 0xA8, 0xFF)
#define C_FIELD_BG      rgba(0x0C, 0x18, 0x2C, 0xE0)
#define C_FIELD_BORDER  rgba(0x30, 0x58, 0x80, 0x80)
#define C_FIELD_FOCUS   rgba(0x40, 0xA0, 0xE0, 0xFF)
#define C_TEXT          rgba(0xC8, 0xE0, 0xF0, 0xFF)
#define C_TEXT_DIM      rgba(0x50, 0x70, 0x90, 0xFF)
#define C_CURSOR        rgba(0x60, 0xD0, 0xFF, 0xFF)
#define C_BTN_TOP       rgb(0x30, 0x80, 0xC0)
#define C_BTN_BOT       rgb(0x20, 0x60, 0x98)
#define C_BTN_TEXT      rgba(0xF0, 0xF8, 0xFF, 0xFF)
#define C_BTN_BORDER    rgba(0x50, 0xA0, 0xD0, 0x40)
#define C_ERROR         rgba(0xE0, 0x50, 0x50, 0xFF)
#define C_FOOTER_TEXT   rgba(0x50, 0x78, 0x98, 0xFF)
#define C_FOOTER_HI     rgba(0x70, 0xA0, 0xC8, 0xFF)
#define C_SPARKLE       rgba(0x80, 0xB0, 0xD0, 0x60)

/* ── Field state ──────────────────────────────────────────────────────── */
#define FIELD_PASS  0
#define FIELD_COUNT 1

typedef struct {
    char buf[64];
    int  len;
    bool active;
} field_t;

static field_t  g_fields[FIELD_COUNT];
static bool     g_error        = false;
static uint32_t g_blink_tick   = 0;
static bool     g_cursor_vis   = true;

/* Cached layout positions for mouse hit-test */
static int g_field_x, g_field_y;
static int g_btn_x, g_btn_y;
static int g_shutdown_x, g_shutdown_y, g_shutdown_w, g_shutdown_h;
static int g_restart_x, g_restart_y, g_restart_w, g_restart_h;

/* ── Helper: integer square root ──────────────────────────────────────── */
static int isqrt(int n)
{
    if (n <= 0) return 0;
    int x = n, y = (x + 1) / 2;
    while (y < x) { x = y; y = (n / y + y) / 2; }
    return x;
}

/* ── Draw scaled character (bitmap font upscaled) ─────────────────────── */
static void draw_char_scaled(canvas_t* c, int x, int y, char ch,
                              int scale, uint32_t color)
{
    const uint8_t* glyph = font_data[(uint8_t)ch];
    for (int row = 0; row < 16; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            if (bits & (0x80 >> col)) {
                /* Draw a scale x scale block */
                int px = x + col * scale;
                int py = y + row * scale;
                for (int sy = 0; sy < scale; sy++) {
                    int ry = py + sy;
                    if (ry < 0 || ry >= c->height) continue;
                    for (int sx = 0; sx < scale; sx++) {
                        int rx = px + sx;
                        if (rx < 0 || rx >= c->width) continue;
                        c->pixels[ry * c->stride + rx] =
                            fb_blend(c->pixels[ry * c->stride + rx], color);
                    }
                }
            }
        }
    }
}

/* ── Draw scaled string ───────────────────────────────────────────────── */
static void draw_string_scaled(canvas_t* c, int x, int y, const char* str,
                                int scale, uint32_t color)
{
    int cx = x;
    while (*str) {
        draw_char_scaled(c, cx, y, *str++, scale, color);
        cx += 8 * scale;
    }
}

static int string_width_scaled(const char* str, int scale)
{
    return (int)strlen(str) * 8 * scale;
}

/* ── Draw background grid pattern ─────────────────────────────────────── */
static void draw_grid(canvas_t* scr, int W, int H)
{
    /* Vertical lines */
    for (int x = GRID_SPACING; x < W; x += GRID_SPACING) {
        for (int y = 0; y < H; y += 2) {  /* dashed for style */
            if ((unsigned)x < (unsigned)scr->width &&
                (unsigned)y < (unsigned)scr->height) {
                uint32_t* px = &scr->pixels[y * scr->stride + x];
                *px = fb_blend(*px, C_GRID);
            }
        }
    }
    /* Horizontal lines */
    for (int y = GRID_SPACING; y < H; y += GRID_SPACING) {
        for (int x = 0; x < W; x += 2) {
            if ((unsigned)x < (unsigned)scr->width &&
                (unsigned)y < (unsigned)scr->height) {
                uint32_t* px = &scr->pixels[y * scr->stride + x];
                *px = fb_blend(*px, C_GRID);
            }
        }
    }
}

/* ── Draw radial glow ─────────────────────────────────────────────────── */
static void draw_glow(canvas_t* scr, int cx, int cy, int radius,
                       uint8_t gr, uint8_t gg, uint8_t gb, int max_alpha)
{
    int x0 = cx - radius; if (x0 < 0) x0 = 0;
    int y0 = cy - radius; if (y0 < 0) y0 = 0;
    int x1 = cx + radius; if (x1 > scr->width)  x1 = scr->width;
    int y1 = cy + radius; if (y1 > scr->height) y1 = scr->height;

    for (int y = y0; y < y1; y++) {
        int dy = y - cy;
        for (int x = x0; x < x1; x++) {
            int dx = x - cx;
            int dist = isqrt(dx * dx + dy * dy);
            if (dist < radius) {
                uint8_t alpha = (uint8_t)(max_alpha * (radius - dist) / radius);
                uint32_t src = ((uint32_t)alpha << 24) |
                               ((uint32_t)gr << 16) |
                               ((uint32_t)gg << 8) | gb;
                uint32_t* px = &scr->pixels[y * scr->stride + x];
                *px = fb_blend(*px, src);
            }
        }
    }
}

/* ── Draw frosted glass card with alpha blending ──────────────────────── */
static void draw_glass_card(canvas_t* scr, int x, int y, int w, int h,
                             int radius)
{
    /* Card shadow - subtle multi-layer */
    for (int s = 6; s >= 1; s--) {
        uint8_t sa = (uint8_t)(12 / s);
        int sx = x + 4 - s;
        int sy = y + 6 - s;
        int sw = w + 2 * s;
        int sh = h + 2 * s;
        /* Draw shadow using alpha rect for each scanline in rounded shape */
        for (int row = 0; row < sh; row++) {
            int ry = sy + row;
            if (ry < 0 || ry >= scr->height) continue;
            for (int col = 0; col < sw; col++) {
                int rx = sx + col;
                if (rx < 0 || rx >= scr->width) continue;
                /* Check rounded corners */
                int r = radius + s;
                bool skip = false;
                if (col < r && row < r) {
                    int dx = r - col - 1, dy = r - row - 1;
                    if (dx * dx + dy * dy > r * r) skip = true;
                } else if (col >= sw - r && row < r) {
                    int dx = col - (sw - r), dy = r - row - 1;
                    if (dx * dx + dy * dy > r * r) skip = true;
                } else if (col < r && row >= sh - r) {
                    int dx = r - col - 1, dy = row - (sh - r);
                    if (dx * dx + dy * dy > r * r) skip = true;
                } else if (col >= sw - r && row >= sh - r) {
                    int dx = col - (sw - r), dy = row - (sh - r);
                    if (dx * dx + dy * dy > r * r) skip = true;
                }
                if (!skip) {
                    uint32_t* px = &scr->pixels[ry * scr->stride + rx];
                    *px = fb_blend(*px, rgba(0, 0, 0, sa));
                }
            }
        }
    }

    /* Card body - semi-transparent with alpha blend */
    for (int row = 0; row < h; row++) {
        int ry = y + row;
        if (ry < 0 || ry >= scr->height) continue;
        for (int col = 0; col < w; col++) {
            int rx = x + col;
            if (rx < 0 || rx >= scr->width) continue;
            /* Rounded corner check */
            bool skip = false;
            if (col < radius && row < radius) {
                int dx = radius - col - 1, dy = radius - row - 1;
                if (dx * dx + dy * dy > radius * radius) skip = true;
            } else if (col >= w - radius && row < radius) {
                int dx = col - (w - radius), dy = radius - row - 1;
                if (dx * dx + dy * dy > radius * radius) skip = true;
            } else if (col < radius && row >= h - radius) {
                int dx = radius - col - 1, dy = row - (h - radius);
                if (dx * dx + dy * dy > radius * radius) skip = true;
            } else if (col >= w - radius && row >= h - radius) {
                int dx = col - (w - radius), dy = row - (h - radius);
                if (dx * dx + dy * dy > radius * radius) skip = true;
            }
            if (!skip) {
                uint32_t* px = &scr->pixels[ry * scr->stride + rx];
                *px = fb_blend(*px, C_CARD_BG);
            }
        }
    }

    /* Card border (1px rounded outline, semi-transparent) */
    /* Top and bottom edges */
    for (int col = radius; col < w - radius; col++) {
        int rx = x + col;
        if (rx >= 0 && rx < scr->width) {
            if (y >= 0 && y < scr->height) {
                uint32_t* px = &scr->pixels[y * scr->stride + rx];
                *px = fb_blend(*px, C_CARD_BORDER);
            }
            int by = y + h - 1;
            if (by >= 0 && by < scr->height) {
                uint32_t* px = &scr->pixels[by * scr->stride + rx];
                *px = fb_blend(*px, C_CARD_BORDER);
            }
        }
    }
    /* Left and right edges */
    for (int row = radius; row < h - radius; row++) {
        int ry = y + row;
        if (ry >= 0 && ry < scr->height) {
            if (x >= 0 && x < scr->width) {
                uint32_t* px = &scr->pixels[ry * scr->stride + x];
                *px = fb_blend(*px, C_CARD_BORDER);
            }
            int bx = x + w - 1;
            if (bx >= 0 && bx < scr->width) {
                uint32_t* px = &scr->pixels[ry * scr->stride + bx];
                *px = fb_blend(*px, C_CARD_BORDER);
            }
        }
    }
    /* Corner arcs */
    for (int row = 0; row < radius; row++) {
        for (int col = 0; col < radius; col++) {
            int dx = radius - col - 1, dy = radius - row - 1;
            int d2 = dx * dx + dy * dy;
            if (d2 <= radius * radius && d2 >= (radius - 1) * (radius - 1)) {
                /* Top-left */
                int px_x = x + col, px_y = y + row;
                if (px_x >= 0 && px_x < scr->width && px_y >= 0 && px_y < scr->height)
                    scr->pixels[px_y * scr->stride + px_x] =
                        fb_blend(scr->pixels[px_y * scr->stride + px_x], C_CARD_BORDER);
                /* Top-right */
                px_x = x + w - 1 - col;
                if (px_x >= 0 && px_x < scr->width && px_y >= 0 && px_y < scr->height)
                    scr->pixels[px_y * scr->stride + px_x] =
                        fb_blend(scr->pixels[px_y * scr->stride + px_x], C_CARD_BORDER);
                /* Bottom-left */
                px_x = x + col; px_y = y + h - 1 - row;
                if (px_x >= 0 && px_x < scr->width && px_y >= 0 && px_y < scr->height)
                    scr->pixels[px_y * scr->stride + px_x] =
                        fb_blend(scr->pixels[px_y * scr->stride + px_x], C_CARD_BORDER);
                /* Bottom-right */
                px_x = x + w - 1 - col;
                if (px_x >= 0 && px_x < scr->width && px_y >= 0 && px_y < scr->height)
                    scr->pixels[px_y * scr->stride + px_x] =
                        fb_blend(scr->pixels[px_y * scr->stride + px_x], C_CARD_BORDER);
            }
        }
    }

    /* Top edge shine (glassmorphism highlight) */
    for (int col = radius; col < w - radius; col++) {
        int rx = x + col;
        int ry = y + 1;
        if (rx >= 0 && rx < scr->width && ry >= 0 && ry < scr->height) {
            uint32_t* px = &scr->pixels[ry * scr->stride + rx];
            *px = fb_blend(*px, C_CARD_SHINE);
        }
    }
}

/* ── Draw 3D sphere avatar ────────────────────────────────────────────── */
static void draw_sphere_avatar(canvas_t* scr, int cx, int cy, int r)
{
    /* Outer ring glow */
    for (int dy = -(r + 4); dy <= (r + 4); dy++) {
        for (int dx = -(r + 4); dx <= (r + 4); dx++) {
            int d2 = dx * dx + dy * dy;
            int outer = (r + 4) * (r + 4);
            int inner = (r + 1) * (r + 1);
            if (d2 <= outer && d2 >= inner) {
                int px = cx + dx, py = cy + dy;
                if (px >= 0 && px < scr->width && py >= 0 && py < scr->height) {
                    uint32_t* p = &scr->pixels[py * scr->stride + px];
                    *p = fb_blend(*p, C_AVATAR_RING);
                }
            }
        }
    }

    /* Main sphere body with radial shading */
    /* Light source: top-left (-0.4, -0.6) */
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            int d2 = dx * dx + dy * dy;
            if (d2 <= r * r) {
                int px = cx + dx, py = cy + dy;
                if (px < 0 || px >= scr->width || py < 0 || py >= scr->height)
                    continue;

                /* Normalized distance from center (0-255) */
                int dist = isqrt(d2) * 255 / r;

                /* Light direction simulation */
                /* Bright spot toward upper-left */
                int lx = dx + r * 35 / 100;  /* offset toward light */
                int ly = dy + r * 45 / 100;
                int ld2 = lx * lx + ly * ly;
                int ldist = isqrt(ld2) * 255 / r;
                if (ldist > 255) ldist = 255;

                /* Base: dark blue sphere */
                int br = 0x10 + (0x30 - 0x10) * (255 - dist) / 255;
                int bg = 0x20 + (0x58 - 0x20) * (255 - dist) / 255;
                int bb = 0x38 + (0x80 - 0x38) * (255 - dist) / 255;

                /* Specular highlight from light direction */
                if (ldist < 120) {
                    int spec = (120 - ldist) * 200 / 120;
                    if (spec > 200) spec = 200;
                    br += spec * (0xC0 - br) / 255;
                    bg += spec * (0xE8 - bg) / 255;
                    bb += spec * (0xF8 - bb) / 255;
                }

                /* Edge darkening (rim) */
                if (dist > 200) {
                    int fade = (dist - 200) * 3;
                    if (fade > 255) fade = 255;
                    br = br * (255 - fade) / 255;
                    bg = bg * (255 - fade) / 255;
                    bb = bb * (255 - fade) / 255;
                }

                if (br > 255) br = 255;
                if (bg > 255) bg = 255;
                if (bb > 255) bb = 255;

                scr->pixels[py * scr->stride + px] =
                    rgb((uint8_t)br, (uint8_t)bg, (uint8_t)bb);
            }
        }
    }

    /* Bright specular reflection spot */
    int hx = cx - r * 30 / 100;
    int hy = cy - r * 35 / 100;
    int hr = r * 18 / 100;
    if (hr < 2) hr = 2;
    for (int dy = -hr; dy <= hr; dy++) {
        for (int dx = -hr; dx <= hr; dx++) {
            int d2 = dx * dx + dy * dy;
            if (d2 <= hr * hr) {
                int px = hx + dx, py = hy + dy;
                if (px >= 0 && px < scr->width && py >= 0 && py < scr->height) {
                    int ldist = isqrt(d2) * 255 / hr;
                    uint8_t alpha = (uint8_t)((255 - ldist) * 180 / 255);
                    uint32_t* p = &scr->pixels[py * scr->stride + px];
                    *p = fb_blend(*p, rgba(0xFF, 0xFF, 0xFF, alpha));
                }
            }
        }
    }
}

/* ── Draw password field with shield icon ─────────────────────────────── */
static void draw_password_field(canvas_t* scr, int x, int y, int w, int h,
                                 field_t* f, bool focused)
{
    /* Field background (alpha-blended) */
    for (int row = 0; row < h; row++) {
        int ry = y + row;
        if (ry < 0 || ry >= scr->height) continue;
        for (int col = 0; col < w; col++) {
            int rx = x + col;
            if (rx < 0 || rx >= scr->width) continue;
            bool skip = false;
            if (col < FIELD_RADIUS && row < FIELD_RADIUS) {
                int dx = FIELD_RADIUS - col - 1, dy = FIELD_RADIUS - row - 1;
                if (dx * dx + dy * dy > FIELD_RADIUS * FIELD_RADIUS) skip = true;
            } else if (col >= w - FIELD_RADIUS && row < FIELD_RADIUS) {
                int dx = col - (w - FIELD_RADIUS), dy = FIELD_RADIUS - row - 1;
                if (dx * dx + dy * dy > FIELD_RADIUS * FIELD_RADIUS) skip = true;
            } else if (col < FIELD_RADIUS && row >= h - FIELD_RADIUS) {
                int dx = FIELD_RADIUS - col - 1, dy = row - (h - FIELD_RADIUS);
                if (dx * dx + dy * dy > FIELD_RADIUS * FIELD_RADIUS) skip = true;
            } else if (col >= w - FIELD_RADIUS && row >= h - FIELD_RADIUS) {
                int dx = col - (w - FIELD_RADIUS), dy = row - (h - FIELD_RADIUS);
                if (dx * dx + dy * dy > FIELD_RADIUS * FIELD_RADIUS) skip = true;
            }
            if (!skip) {
                uint32_t* px = &scr->pixels[ry * scr->stride + rx];
                *px = fb_blend(*px, C_FIELD_BG);
            }
        }
    }

    /* Border */
    uint32_t brd = focused ? C_FIELD_FOCUS : C_FIELD_BORDER;
    draw_rect_rounded_outline(scr, x, y, w, h, FIELD_RADIUS, 1, brd);

    /* Focus glow line at bottom */
    if (focused) {
        for (int col = FIELD_RADIUS; col < w - FIELD_RADIUS; col++) {
            int rx = x + col;
            int ry = y + h - 2;
            if (rx >= 0 && rx < scr->width && ry >= 0 && ry < scr->height) {
                uint32_t* px = &scr->pixels[ry * scr->stride + rx];
                *px = fb_blend(*px, rgba(0x40, 0xA0, 0xE0, 0x80));
            }
        }
    }

    /* Shield icon in left area */
    int icon_cx = x + 20;
    int icon_cy = y + h / 2;
    /* Simple shield shape - small upward triangle + rect */
    /* Top chevron (^) */
    for (int i = 0; i < 5; i++) {
        int px1 = icon_cx - 4 + i, py1 = icon_cy - 5 + i;
        int px2 = icon_cx + 4 - i;
        if (py1 >= 0 && py1 < scr->height) {
            if (px1 >= 0 && px1 < scr->width)
                scr->pixels[py1 * scr->stride + px1] =
                    fb_blend(scr->pixels[py1 * scr->stride + px1], C_FIELD_FOCUS);
            if (px2 >= 0 && px2 < scr->width)
                scr->pixels[py1 * scr->stride + px2] =
                    fb_blend(scr->pixels[py1 * scr->stride + px2], C_FIELD_FOCUS);
        }
    }
    /* Vertical line under chevron */
    for (int i = 0; i < 5; i++) {
        int py = icon_cy + i;
        if (icon_cx >= 0 && icon_cx < scr->width && py >= 0 && py < scr->height)
            scr->pixels[py * scr->stride + icon_cx] =
                fb_blend(scr->pixels[py * scr->stride + icon_cx], C_FIELD_FOCUS);
    }

    /* Separator line after icon */
    for (int row = 8; row < h - 8; row++) {
        int rx = x + 34;
        int ry = y + row;
        if (rx >= 0 && rx < scr->width && ry >= 0 && ry < scr->height) {
            uint32_t* px = &scr->pixels[ry * scr->stride + rx];
            *px = fb_blend(*px, rgba(0x40, 0x60, 0x80, 0x60));
        }
    }

    /* Password dots or placeholder */
    int tx = x + FIELD_PAD;
    int ty = y + (h - FONT_H) / 2;

    if (f->len == 0 && !focused) {
        draw_string(scr, tx, ty, "Enter password", C_TEXT_DIM, rgba(0,0,0,0));
    } else if (f->len > 0) {
        /* Draw bullet dots for each character */
        for (int i = 0; i < f->len && i < 32; i++) {
            int dot_cx = tx + i * 12 + 4;
            int dot_cy = y + h / 2;
            draw_circle_filled(scr, dot_cx, dot_cy, 3, C_TEXT);
        }
    }

    /* Blinking cursor */
    if (focused && g_cursor_vis) {
        int cw = f->len * 12;
        if (f->len == 0) cw = 0;
        int cursor_x = tx + cw + (f->len > 0 ? 4 : 0);
        draw_rect(scr, cursor_x, ty, 2, FONT_H, C_CURSOR);
    }
}

/* ── Draw AUTHENTICATE button ─────────────────────────────────────────── */
static void draw_auth_button(canvas_t* scr, int x, int y, int w, int h)
{
    /* Button gradient background */
    draw_gradient_v(scr, x + BTN_RADIUS, y, w - 2 * BTN_RADIUS, h,
                    C_BTN_TOP, C_BTN_BOT);
    /* Left and right edges */
    draw_gradient_v(scr, x, y + BTN_RADIUS, BTN_RADIUS, h - 2 * BTN_RADIUS,
                    C_BTN_TOP, C_BTN_BOT);
    draw_gradient_v(scr, x + w - BTN_RADIUS, y + BTN_RADIUS,
                    BTN_RADIUS, h - 2 * BTN_RADIUS, C_BTN_TOP, C_BTN_BOT);

    /* Rounded corners (quarter circles with gradient) */
    for (int row = 0; row < BTN_RADIUS; row++) {
        /* Interpolate color for this row */
        int t = row * 255 / h;
        uint8_t cr = (uint8_t)(0x30 + (0x20 - 0x30) * t / 255);
        uint8_t cg = (uint8_t)(0x80 + (0x60 - 0x80) * t / 255);
        uint8_t cb = (uint8_t)(0xC0 + (0x98 - 0xC0) * t / 255);
        uint32_t c = rgb(cr, cg, cb);

        for (int col = 0; col < BTN_RADIUS; col++) {
            int dx = BTN_RADIUS - col - 1, dy = BTN_RADIUS - row - 1;
            if (dx * dx + dy * dy <= BTN_RADIUS * BTN_RADIUS) {
                /* Top-left */
                if (x + col >= 0 && x + col < scr->width &&
                    y + row >= 0 && y + row < scr->height)
                    scr->pixels[(y + row) * scr->stride + x + col] = c;
                /* Top-right */
                if (x + w - 1 - col >= 0 && x + w - 1 - col < scr->width &&
                    y + row >= 0 && y + row < scr->height)
                    scr->pixels[(y + row) * scr->stride + x + w - 1 - col] = c;
            }
        }

        /* Bottom corners */
        int brow = h - 1 - row;
        t = brow * 255 / h;
        cr = (uint8_t)(0x30 + (0x20 - 0x30) * t / 255);
        cg = (uint8_t)(0x80 + (0x60 - 0x80) * t / 255);
        cb = (uint8_t)(0xC0 + (0x98 - 0xC0) * t / 255);
        c = rgb(cr, cg, cb);

        for (int col = 0; col < BTN_RADIUS; col++) {
            int dx = BTN_RADIUS - col - 1, dy = BTN_RADIUS - row - 1;
            if (dx * dx + dy * dy <= BTN_RADIUS * BTN_RADIUS) {
                if (x + col >= 0 && x + col < scr->width &&
                    y + brow >= 0 && y + brow < scr->height)
                    scr->pixels[(y + brow) * scr->stride + x + col] = c;
                if (x + w - 1 - col >= 0 && x + w - 1 - col < scr->width &&
                    y + brow >= 0 && y + brow < scr->height)
                    scr->pixels[(y + brow) * scr->stride + x + w - 1 - col] = c;
            }
        }
    }

    /* Subtle border */
    draw_rect_rounded_outline(scr, x, y, w, h, BTN_RADIUS, 1, C_BTN_BORDER);

    /* Button text */
    const char* lbl = "AUTHENTICATE";
    int tw = draw_string_width(lbl);
    draw_string(scr, x + (w - tw) / 2, y + (h - FONT_H) / 2,
                lbl, C_BTN_TEXT, rgba(0,0,0,0));
}

/* ── Draw 4-point decorative sparkle/star ─────────────────────────────── */
static void draw_sparkle(canvas_t* scr, int cx, int cy, int size)
{
    /* Vertical line */
    for (int i = -size; i <= size; i++) {
        int py = cy + i;
        if (cx >= 0 && cx < scr->width && py >= 0 && py < scr->height) {
            int dist = i < 0 ? -i : i;
            uint8_t alpha = (uint8_t)((size - dist) * 100 / size);
            uint32_t* px = &scr->pixels[py * scr->stride + cx];
            *px = fb_blend(*px, rgba(0xB0, 0xD0, 0xE8, alpha));
        }
    }
    /* Horizontal line */
    for (int i = -size; i <= size; i++) {
        int px_x = cx + i;
        if (px_x >= 0 && px_x < scr->width && cy >= 0 && cy < scr->height) {
            int dist = i < 0 ? -i : i;
            uint8_t alpha = (uint8_t)((size - dist) * 100 / size);
            uint32_t* px = &scr->pixels[cy * scr->stride + px_x];
            *px = fb_blend(*px, rgba(0xB0, 0xD0, 0xE8, alpha));
        }
    }
    /* Diagonal lines (shorter) */
    int ds = size * 60 / 100;
    for (int i = -ds; i <= ds; i++) {
        int dist = i < 0 ? -i : i;
        uint8_t alpha = (uint8_t)((ds - dist) * 60 / ds);
        uint32_t col = rgba(0xB0, 0xD0, 0xE8, alpha);

        int px1 = cx + i, py1 = cy + i;
        if (px1 >= 0 && px1 < scr->width && py1 >= 0 && py1 < scr->height)
            scr->pixels[py1 * scr->stride + px1] =
                fb_blend(scr->pixels[py1 * scr->stride + px1], col);

        int px2 = cx + i, py2 = cy - i;
        if (px2 >= 0 && px2 < scr->width && py2 >= 0 && py2 < scr->height)
            scr->pixels[py2 * scr->stride + px2] =
                fb_blend(scr->pixels[py2 * scr->stride + px2], col);
    }
    /* Bright center dot */
    if (cx >= 0 && cx < scr->width && cy >= 0 && cy < scr->height) {
        scr->pixels[cy * scr->stride + cx] = rgb(0xE0, 0xF0, 0xFF);
    }
}

/* ── Day-of-week name ─────────────────────────────────────────────────── */
static const char* day_names[] = {
    "THU", "FRI", "SAT", "SUN", "MON", "TUE", "WED"
};

/* ── Main login screen renderer ───────────────────────────────────────── */
static void draw_login_screen(canvas_t* scr)
{
    int W = scr->width;
    int H = scr->height;

    /* ── 1. Full-screen gradient background ─────────────────────────── */
    draw_gradient_v(scr, 0, 0, W, H, C_BG_TOP, C_BG_BOT);

    /* ── 2. Subtle grid pattern ─────────────────────────────────────── */
    draw_grid(scr, W, H);

    /* ── 3. Large radial glow centered slightly left ────────────────── */
    draw_glow(scr, W * 35 / 100, H * 40 / 100, H * 80 / 100,
              0x15, 0x30, 0x60, 25);

    /* Smaller accent glow near the card */
    draw_glow(scr, W * 70 / 100, H * 45 / 100, H * 40 / 100,
              0x20, 0x50, 0x80, 15);

    /* ── 4. LEFT SIDE: Large clock ──────────────────────────────────── */
    uint32_t secs = timer_get_ticks() / TIMER_FREQ;
    uint32_t hours   = (secs / 3600) % 24;
    uint32_t minutes = (secs / 60)   % 60;
    uint32_t seconds = secs % 60;

    char time_str[8];
    time_str[0] = '0' + (char)(hours / 10);
    time_str[1] = '0' + (char)(hours % 10);
    time_str[2] = ':';
    time_str[3] = '0' + (char)(minutes / 10);
    time_str[4] = '0' + (char)(minutes % 10);
    time_str[5] = '\0';

    /* Clock position: left side, vertically centered */
    int left_margin = W * 8 / 100;
    int clock_scale = 6;  /* 6x scale = 48x96 per character */
    if (W < 800) clock_scale = 4;
    int clock_y = H * 25 / 100;
    /* Make colon blink with seconds */
    uint32_t colon_color = (seconds % 2 == 0) ? C_CLOCK : C_CLOCK_DIM;

    /* Draw hours */
    draw_char_scaled(scr, left_margin, clock_y, time_str[0],
                     clock_scale, C_CLOCK);
    draw_char_scaled(scr, left_margin + 8 * clock_scale, clock_y, time_str[1],
                     clock_scale, C_CLOCK);
    /* Draw colon */
    draw_char_scaled(scr, left_margin + 16 * clock_scale, clock_y, ':',
                     clock_scale, colon_color);
    /* Draw minutes */
    draw_char_scaled(scr, left_margin + 24 * clock_scale, clock_y, time_str[3],
                     clock_scale, C_CLOCK);
    draw_char_scaled(scr, left_margin + 32 * clock_scale, clock_y, time_str[4],
                     clock_scale, C_CLOCK);

    /* ── 5. Date line below clock ───────────────────────────────────── */
    int date_y = clock_y + 16 * clock_scale + 12;
    int date_scale = 2;

    /* Calculate day of week from uptime (not real date, but looks good) */
    uint32_t day_idx = (secs / 86400) % 7;

    char date_str[32];
    /* Static date display - formatted like reference */
    date_str[0] = '2'; date_str[1] = '0'; date_str[2] = '2'; date_str[3] = '6';
    date_str[4] = '.';
    date_str[5] = '0'; date_str[6] = '3';
    date_str[7] = '.';
    date_str[8] = '1'; date_str[9] = '5';
    date_str[10] = ' '; date_str[11] = ' ';
    /* Day name */
    const char* dn = day_names[day_idx];
    date_str[12] = dn[0]; date_str[13] = dn[1]; date_str[14] = dn[2];
    date_str[15] = '\0';

    draw_string_scaled(scr, left_margin, date_y, date_str, date_scale, C_DATE);

    /* ── 6. System info lines ───────────────────────────────────────── */
    int info_y = date_y + 16 * date_scale + 16;
    draw_string(scr, left_margin, info_y,
                OS_NAME " | Kernel v" OS_VERSION " | System Secure | Ready",
                C_INFO, rgba(0,0,0,0));
    draw_string(scr, left_margin, info_y + 20,
                "Device: AETHER-WS-01 | " OS_RELEASE " | Encrypted",
                C_INFO_DIM, rgba(0,0,0,0));

    /* ── 7. Status indicators (small icons as text) ─────────────────── */
    int icon_y = info_y + 48;
    /* WiFi-like bars */
    for (int i = 0; i < 4; i++) {
        int bar_h = 3 + i * 3;
        int bx = left_margin + i * 5;
        int by = icon_y + 12 - bar_h;
        draw_rect(scr, bx, by, 3, bar_h, C_INFO);
    }
    /* Separator */
    draw_rect(scr, left_margin + 26, icon_y + 2, 1, 10, C_INFO_DIM);
    /* Signal bars */
    for (int i = 0; i < 4; i++) {
        int bar_h = 4 + i * 2;
        int bx = left_margin + 32 + i * 4;
        int by = icon_y + 12 - bar_h;
        draw_rect(scr, bx, by, 2, bar_h, C_INFO);
    }

    /* ── 8. RIGHT SIDE: Frosted glass login card ────────────────────── */
    int card_x = W - CARD_W - W * 10 / 100;
    int card_y = H * 18 / 100;

    /* Clamp card to reasonable position */
    if (card_x < W / 2) card_x = W / 2;

    draw_glass_card(scr, card_x, card_y, CARD_W, CARD_H, CARD_RADIUS);

    /* ── 9. Avatar sphere ───────────────────────────────────────────── */
    int avatar_cx = card_x + 70;
    int avatar_cy = card_y + 65;
    draw_sphere_avatar(scr, avatar_cx, avatar_cy, AVATAR_R);

    /* ── 10. Username and info ──────────────────────────────────────── */
    int name_x = avatar_cx + AVATAR_R + 20;
    int name_y = avatar_cy - 16;

    /* Display the username from the field (or default) */
    draw_string_scaled(scr, name_x, name_y, login_username, 2, C_NAME);

    /* Last login info */
    char login_info[64];
    strncpy(login_info, "Last Login: ", 63);
    /* Use uptime to fake a timestamp */
    strncat(login_info, "2026.03.15 ", 63 - strlen(login_info));
    char ts[16];
    ts[0] = '0' + (char)(hours / 10); ts[1] = '0' + (char)(hours % 10);
    ts[2] = ':';
    ts[3] = '0' + (char)(minutes / 10); ts[4] = '0' + (char)(minutes % 10);
    ts[5] = ' '; ts[6] = 'U'; ts[7] = 'T'; ts[8] = 'C'; ts[9] = '\0';
    strncat(login_info, ts, 63 - strlen(login_info));
    login_info[63] = '\0';
    draw_string(scr, name_x, name_y + 36, login_info, C_SUBTEXT, rgba(0,0,0,0));

    /* ── 11. Password field ─────────────────────────────────────────── */
    int field_x = card_x + (CARD_W - FIELD_W) / 2;
    int field_y = avatar_cy + AVATAR_R + 40;
    draw_password_field(scr, field_x, field_y, FIELD_W, FIELD_H,
                        &g_fields[FIELD_PASS], true);
    g_field_x = field_x;
    g_field_y = field_y;

    /* ── 12. Error message ──────────────────────────────────────────── */
    int err_space = 0;
    if (g_error) {
        const char* err = "Authentication failed. Try again.";
        int ew = draw_string_width(err);
        draw_string(scr, card_x + (CARD_W - ew) / 2, field_y + FIELD_H + 6,
                    err, C_ERROR, rgba(0,0,0,0));
        err_space = FONT_H + 6;
    }

    /* ── 13. AUTHENTICATE button ────────────────────────────────────── */
    int btn_x = card_x + (CARD_W - BTN_W) / 2;
    int btn_y = field_y + FIELD_H + 14 + err_space;
    draw_auth_button(scr, btn_x, btn_y, BTN_W, BTN_H);
    g_btn_x = btn_x;
    g_btn_y = btn_y;

    /* ── 14. Footer below card ──────────────────────────────────────── */
    int footer_y = card_y + CARD_H + 20;
    int footer_cx = card_x + CARD_W / 2;

    /* Separator line */
    int sep_w = CARD_W - 40;
    for (int i = 0; i < sep_w; i++) {
        int rx = card_x + 20 + i;
        if (rx >= 0 && rx < scr->width && footer_y - 8 >= 0 &&
            footer_y - 8 < scr->height) {
            uint32_t* px = &scr->pixels[(footer_y - 8) * scr->stride + rx];
            *px = fb_blend(*px, rgba(0x40, 0x60, 0x80, 0x30));
        }
    }

    /* Shutdown / Restart / Sleep links */
    const char* s1 = "Shutdown";
    const char* s2 = "Restart";
    const char* s3 = "Sleep";
    int total_w = draw_string_width(s1) + draw_string_width(s2) +
                  draw_string_width(s3) + 48;
    int sx = footer_cx - total_w / 2;

    /* Power icon (small circle with line) */
    draw_circle(scr, sx - 12, footer_y + 7, 5, C_FOOTER_TEXT);
    draw_vline(scr, sx - 12, footer_y, 5, C_FOOTER_TEXT);

    g_shutdown_x = sx;
    g_shutdown_y = footer_y;
    g_shutdown_w = draw_string_width(s1);
    g_shutdown_h = FONT_H;
    draw_string(scr, sx, footer_y, s1, C_FOOTER_HI, rgba(0,0,0,0));
    sx += draw_string_width(s1) + 8;

    draw_string(scr, sx, footer_y, "/", C_FOOTER_TEXT, rgba(0,0,0,0));
    sx += 16;

    g_restart_x = sx;
    g_restart_y = footer_y;
    g_restart_w = draw_string_width(s2);
    g_restart_h = FONT_H;
    draw_string(scr, sx, footer_y, s2, C_FOOTER_HI, rgba(0,0,0,0));
    sx += draw_string_width(s2) + 8;

    draw_string(scr, sx, footer_y, "/", C_FOOTER_TEXT, rgba(0,0,0,0));
    sx += 16;

    draw_string(scr, sx, footer_y, s3, C_FOOTER_HI, rgba(0,0,0,0));

    /* Keyboard layout indicator */
    draw_string(scr, card_x + CARD_W - 100, footer_y + 24,
                "Keyboard: EN-US", C_FOOTER_TEXT, rgba(0,0,0,0));

    /* ── 15. Decorative sparkle (bottom-right) ──────────────────────── */
    draw_sparkle(scr, W - W * 8 / 100, H - H * 10 / 100, 20);

    /* Smaller sparkle */
    draw_sparkle(scr, W - W * 12 / 100, H - H * 18 / 100, 10);

    /* ── 16. OS version stamp (bottom-left) ─────────────────────────── */
    draw_string(scr, left_margin, H - 30, OS_BANNER_SHORT,
                C_INFO_DIM, rgba(0,0,0,0));

    /* ── 17. Uptime clock (bottom-right) ────────────────────────────── */
    char uptime[16];
    uptime[0] = '0' + (char)(hours / 10);   uptime[1] = '0' + (char)(hours % 10);
    uptime[2] = ':';
    uptime[3] = '0' + (char)(minutes / 10); uptime[4] = '0' + (char)(minutes % 10);
    uptime[5] = ':';
    uptime[6] = '0' + (char)(seconds / 10); uptime[7] = '0' + (char)(seconds % 10);
    uptime[8] = '\0';
    int uw = draw_string_width(uptime);
    draw_string(scr, W - uw - 20, H - 30, uptime,
                C_INFO, rgba(0,0,0,0));
}

/* ── Authentication ───────────────────────────────────────────────────── */
static bool try_login(void)
{
    if (strlen(login_username) < 1) return false;
    int uid = users_authenticate(login_username, g_fields[FIELD_PASS].buf);
    if (uid < 0) return false;
    return true;
}

/* ── Main login loop ──────────────────────────────────────────────────── */
void login_run(void)
{
    if (!fb_ready()) return;

    canvas_t screen = draw_main_canvas();

    /* Initialize password field */
    g_fields[FIELD_PASS].buf[0] = '\0';
    g_fields[FIELD_PASS].len    = 0;
    g_fields[FIELD_PASS].active = true;
    g_error        = false;
    g_blink_tick   = timer_get_ticks();
    g_cursor_vis   = true;

    uint8_t prev_kb_char = 0;
    bool    prev_enter   = false;

    while (1) {
        /* Cursor blink */
        uint32_t now = timer_get_ticks();
        if (now - g_blink_tick >= (uint32_t)(TIMER_FREQ / 2)) {
            g_cursor_vis = !g_cursor_vis;
            g_blink_tick = now;
        }

        /* --- Keyboard input --- */
        char ch = keyboard_read();
        if (ch && ch != (char)prev_kb_char) {
            field_t* f = &g_fields[FIELD_PASS];
            if (ch == '\n' || ch == '\r') {
                if (!prev_enter) {
                    prev_enter = true;
                    if (try_login()) return;
                    g_error = true;
                    f->len = 0;
                    f->buf[0] = '\0';
                }
            } else {
                prev_enter = false;
                if (ch == '\b') {
                    if (f->len > 0) {
                        f->buf[--f->len] = '\0';
                        g_error = false;
                    }
                } else if (ch >= 0x20 && ch < 0x7F && f->len < 63) {
                    f->buf[f->len++] = ch;
                    f->buf[f->len]   = '\0';
                    g_error = false;
                }
            }
        } else {
            prev_enter = false;
        }
        prev_kb_char = (uint8_t)ch;

        /* --- Mouse input --- */
        {
            mouse_event_t mev;
            mouse_get_event(&mev);
            if (mev.left_clicked) {
                int mx = mev.x, my = mev.y;

                /* Click on AUTHENTICATE button */
                if (mx >= g_btn_x && mx < g_btn_x + BTN_W &&
                    my >= g_btn_y && my < g_btn_y + BTN_H) {
                    if (try_login()) return;
                    g_error = true;
                    g_fields[FIELD_PASS].len = 0;
                    g_fields[FIELD_PASS].buf[0] = '\0';
                }
            }
        }

        cursor_erase();
        draw_login_screen(&screen);
        cursor_render();
        fb_flip();
        scheduler_yield();
    }
}
