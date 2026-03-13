/*
 * services/login.c - AetherOS graphical login screen
 *
 * Renders a modern, polished full-screen login UI with:
 *   - Rich gradient background with radial glow
 *   - Frosted-glass card with drop shadow
 *   - Professional avatar with accent ring
 *   - Rounded input fields with focus indicators
 *   - Gradient sign-in button
 *   - Bottom status bar with clock and branding
 *   - Full mouse + keyboard input support
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

/* ── Layout constants ─────────────────────────────────────────────────── */
#define CARD_W       420
#define CARD_H       380
#define CARD_RADIUS  12
#define FIELD_W      320
#define FIELD_H      36
#define FIELD_RADIUS 6
#define FIELD_PAD    10
#define BTN_W        320
#define BTN_H        40
#define BTN_RADIUS   8
#define AVATAR_R     32
#define AVATAR_RING  3
#define SHADOW_OFF   6
#define SHADOW_BLUR  4
#define STATUSBAR_H  32

/* ── Field state ──────────────────────────────────────────────────────── */
#define FIELD_USER  0
#define FIELD_PASS  1
#define FIELD_COUNT 2

typedef struct {
    char buf[64];
    int  len;
    bool active;
} field_t;

static field_t  g_fields[FIELD_COUNT];
static int      g_active_field = 0;
static bool     g_error        = false;
static uint32_t g_blink_tick   = 0;
static bool     g_cursor_vis   = true;

/* Cached layout positions (computed once per frame, used by mouse hit-test) */
static int g_fx, g_fy1, g_fy2, g_btn_x, g_btn_y;

/* ── Helper: integer square root (for radial glow) ────────────────────── */
static int isqrt(int n)
{
    if (n <= 0) return 0;
    int x = n, y = (x + 1) / 2;
    while (y < x) { x = y; y = (n / y + y) / 2; }
    return x;
}

/* ── Draw the radial glow on the background ───────────────────────────── */
static void draw_bg_glow(canvas_t* scr, int cx, int cy, int radius,
                          uint32_t glow_color)
{
    uint8_t gr = (glow_color >> 16) & 0xFF;
    uint8_t gg = (glow_color >>  8) & 0xFF;
    uint8_t gb =  glow_color        & 0xFF;

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
                uint8_t alpha = (uint8_t)(40 * (radius - dist) / radius);
                uint32_t src = ((uint32_t)alpha << 24) | ((uint32_t)gr << 16) |
                               ((uint32_t)gg << 8) | gb;
                uint32_t* px = &scr->pixels[y * scr->stride + x];
                *px = fb_blend(*px, src);
            }
        }
    }
}

/* ── Draw the user avatar ─────────────────────────────────────────────── */
static void draw_avatar(canvas_t* scr, int cx, int cy, const theme_t* th)
{
    /* Outer ring */
    draw_circle_filled(scr, cx, cy, AVATAR_R + AVATAR_RING, th->login_avatar_ring);
    /* Inner fill */
    draw_circle_filled(scr, cx, cy, AVATAR_R, th->login_avatar_bg);
    /* Silhouette: head */
    int head_r = AVATAR_R * 38 / 100;
    draw_circle_filled(scr, cx, cy - AVATAR_R / 5, head_r, th->login_avatar_ring);
    /* Silhouette: body (shoulders arc) */
    int body_r = AVATAR_R * 55 / 100;
    int body_cy = cy + AVATAR_R / 2 + body_r / 3;
    draw_circle_filled(scr, cx, body_cy, body_r, th->login_avatar_ring);
}

/* ── Draw a single input field ────────────────────────────────────────── */
static void draw_field(canvas_t* scr, int x, int y, int w, int h,
                       const char* label, const char* placeholder,
                       field_t* f, bool is_password, bool active,
                       const theme_t* th)
{
    /* Label above field */
    draw_string(scr, x, y - FONT_H - 6, label, th->login_text, rgba(0,0,0,0));

    /* Field background with rounded corners */
    draw_rect_rounded(scr, x, y, w, h, FIELD_RADIUS, th->login_field_bg);

    /* Border */
    uint32_t brd = active ? th->login_field_focus : th->login_field_border;
    int brd_thick = active ? 2 : 1;
    draw_rect_rounded_outline(scr, x, y, w, h, FIELD_RADIUS, brd_thick, brd);

    /* Focus accent line at bottom */
    if (active) {
        draw_rect_rounded(scr, x + 2, y + h - 3, w - 4, 2, 1, th->login_field_focus);
    }

    /* Text content */
    char display[64];
    if (is_password) {
        int i;
        for (i = 0; i < f->len && i < 63; i++) display[i] = '*';
        display[i] = '\0';
    } else {
        strncpy(display, f->buf, 63);
        display[63] = '\0';
    }

    int tx = x + FIELD_PAD;
    int ty = y + (h - FONT_H) / 2;

    if (f->len == 0 && !active && placeholder) {
        draw_string(scr, tx, ty, placeholder, th->login_text_dim, rgba(0,0,0,0));
    } else {
        draw_string(scr, tx, ty, display, th->login_text, rgba(0,0,0,0));
    }

    /* Blinking cursor */
    if (active && g_cursor_vis) {
        int cw = (int)strlen(display) * FONT_W;
        draw_rect(scr, tx + cw + 1, ty, 2, FONT_H, th->login_cursor);
    }
}

/* ── Main login screen renderer ───────────────────────────────────────── */
static void draw_login_screen(canvas_t* scr)
{
    const theme_t* th = theme_current();
    int W = scr->width;
    int H = scr->height;
    int cx = W / 2;
    int cy = H / 2 - 10;

    /* ── 1. Full-screen gradient background ─────────────────────────── */
    draw_gradient_v(scr, 0, 0, W, H, th->login_bg, th->login_bg2);

    /* ── 2. Subtle radial glow from center ──────────────────────────── */
    int glow_r = (W < H ? W : H) * 2 / 3;
    draw_bg_glow(scr, cx, cy - 40, glow_r, th->login_glow);

    /* ── 3. Subtle dot pattern for texture ──────────────────────────── */
    for (int py = 8; py < H; py += 32) {
        for (int px = 8; px < W; px += 32) {
            draw_pixel(scr, px, py, rgba(0xFF, 0xFF, 0xFF, 0x06));
        }
    }

    /* ── 4. Drop shadow behind card ─────────────────────────────────── */
    int card_x = cx - CARD_W / 2;
    int card_y = cy - CARD_H / 2;

    for (int s = SHADOW_BLUR; s >= 1; s--) {
        uint8_t sa = (uint8_t)(20 / s);
        draw_rect_rounded(scr, card_x + SHADOW_OFF - s, card_y + SHADOW_OFF - s,
                          CARD_W + 2 * s, CARD_H + 2 * s,
                          CARD_RADIUS + s, rgba(0, 0, 0, sa));
    }

    /* ── 5. Card background ─────────────────────────────────────────── */
    draw_rect_rounded(scr, card_x, card_y, CARD_W, CARD_H,
                      CARD_RADIUS, th->login_box);
    draw_rect_rounded_outline(scr, card_x, card_y, CARD_W, CARD_H,
                               CARD_RADIUS, 1, th->login_box_border);

    /* ── 6. Avatar at top of card ───────────────────────────────────── */
    int avatar_cy = card_y + 52;
    draw_avatar(scr, cx, avatar_cy, th);

    /* ── 7. "Welcome" greeting ──────────────────────────────────────── */
    int greet_y = avatar_cy + AVATAR_R + AVATAR_RING + 14;
    const char* greet = "Welcome to " OS_NAME;
    int gw = (int)strlen(greet) * FONT_W;
    draw_string(scr, cx - gw / 2, greet_y, greet, th->login_text, rgba(0,0,0,0));

    const char* sub = "Sign in to continue";
    int subw = (int)strlen(sub) * FONT_W;
    draw_string(scr, cx - subw / 2, greet_y + FONT_H + 2, sub,
                th->login_text_dim, rgba(0,0,0,0));

    /* ── 8. Separator line ──────────────────────────────────────────── */
    int sep_y = greet_y + FONT_H * 2 + 12;
    int sep_margin = 40;
    draw_hline(scr, card_x + sep_margin, sep_y, CARD_W - 2 * sep_margin,
               th->login_separator);

    /* ── 9. Username field ──────────────────────────────────────────── */
    int fx = cx - FIELD_W / 2;
    int fy1 = sep_y + 16;
    draw_field(scr, fx, fy1, FIELD_W, FIELD_H,
               "Username", "Enter your username",
               &g_fields[FIELD_USER], false,
               g_active_field == FIELD_USER, th);

    /* ── 10. Password field ─────────────────────────────────────────── */
    int fy2 = fy1 + FIELD_H + 28;
    draw_field(scr, fx, fy2, FIELD_W, FIELD_H,
               "Password", "Enter your password",
               &g_fields[FIELD_PASS], true,
               g_active_field == FIELD_PASS, th);

    /* ── 11. Error message ──────────────────────────────────────────── */
    int err_space = 0;
    if (g_error) {
        const char* err = "Invalid credentials. Please try again.";
        int ew = (int)strlen(err) * FONT_W;
        draw_string(scr, cx - ew / 2, fy2 + FIELD_H + 6, err,
                    th->error, rgba(0,0,0,0));
        err_space = FONT_H + 4;
    }

    /* ── 12. Sign-in button ─────────────────────────────────────────── */
    int btn_y = fy2 + FIELD_H + 12 + err_space;
    int btn_x = cx - BTN_W / 2;

    draw_rect_rounded(scr, btn_x, btn_y, BTN_W, BTN_H, BTN_RADIUS,
                      th->login_btn_bg);
    /* Bottom half slightly darker for gradient effect */
    for (int row = BTN_H / 2; row < BTN_H; row++) {
        int ry = btn_y + row;
        if (ry < 0 || ry >= scr->height) continue;
        for (int col = 0; col < BTN_W; col++) {
            int rx = btn_x + col;
            if (rx < 0 || rx >= scr->width) continue;
            int ddx = 0, ddy = 0;
            if (col < BTN_RADIUS) ddx = BTN_RADIUS - col;
            else if (col > BTN_W - BTN_RADIUS) ddx = col - (BTN_W - BTN_RADIUS);
            if (row > BTN_H - BTN_RADIUS) ddy = row - (BTN_H - BTN_RADIUS);
            if (ddx * ddx + ddy * ddy > BTN_RADIUS * BTN_RADIUS) continue;
            uint32_t* px = &scr->pixels[ry * scr->stride + rx];
            *px = fb_blend(*px, rgba(0, 0, 0, 0x18));
        }
    }

    const char* btn_lbl = "Sign In";
    int blw = (int)strlen(btn_lbl) * FONT_W;
    draw_string(scr, cx - blw / 2, btn_y + (BTN_H - FONT_H) / 2,
                btn_lbl, th->text_on_accent, rgba(0,0,0,0));

    /* Cache layout positions for mouse hit-testing */
    g_fx = fx; g_fy1 = fy1; g_fy2 = fy2;
    g_btn_x = btn_x; g_btn_y = btn_y;

    /* ── 13. Keyboard hint below button ─────────────────────────────── */
    const char* hint = "Press Enter to sign in  |  Tab to switch fields";
    int hiw = (int)strlen(hint) * FONT_W;
    draw_string(scr, cx - hiw / 2, btn_y + BTN_H + 10, hint,
                th->login_text_dim, rgba(0,0,0,0));

    /* ── 14. Bottom status bar ──────────────────────────────────────── */
    int bar_y = H - STATUSBAR_H;
    draw_rect_alpha(scr, 0, bar_y, W, STATUSBAR_H, rgba(0, 0, 0, 0x30));
    draw_hline(scr, 0, bar_y, W, rgba(0xFF, 0xFF, 0xFF, 0x0A));

    /* OS branding on left */
    const char* brand = OS_BANNER_SHORT;
    int brand_y = bar_y + (STATUSBAR_H - FONT_H) / 2;
    draw_string(scr, 12, brand_y, brand, rgba(0xA0, 0xB8, 0xD0, 0xFF),
                rgba(0,0,0,0));

    /* Clock on right */
    uint32_t secs = timer_get_ticks() / TIMER_FREQ;
    char clock_str[16];
    uint32_t h2 = (secs / 3600) % 24;
    uint32_t m2 = (secs / 60)   % 60;
    uint32_t s2 =  secs         % 60;
    clock_str[0] = '0' + (char)(h2 / 10); clock_str[1] = '0' + (char)(h2 % 10);
    clock_str[2] = ':';
    clock_str[3] = '0' + (char)(m2 / 10); clock_str[4] = '0' + (char)(m2 % 10);
    clock_str[5] = ':';
    clock_str[6] = '0' + (char)(s2 / 10); clock_str[7] = '0' + (char)(s2 % 10);
    clock_str[8] = '\0';
    int clk_w = 8 * FONT_W;
    draw_string(scr, W - clk_w - 12, brand_y, clock_str,
                rgba(0xD0, 0xE0, 0xF0, 0xFF), rgba(0,0,0,0));

    /* Mouse debug: show coordinates top-left so we know if IRQ 12 fires */
    {
        char mdbg[32];
        int mx = mouse.x, my = mouse.y;
        int i = 0;
        mdbg[i++] = 'M'; mdbg[i++] = ':';
        if (mx < 0) { mdbg[i++] = '-'; mx = -mx; }
        int tmp = mx; int digits = 1;
        while (tmp >= 10) { tmp /= 10; digits++; }
        for (int d = digits - 1; d >= 0; d--) {
            mdbg[i + d] = '0' + (char)(mx % 10); mx /= 10;
        }
        i += digits;
        mdbg[i++] = ',';
        if (my < 0) { mdbg[i++] = '-'; my = -my; }
        tmp = my; digits = 1;
        while (tmp >= 10) { tmp /= 10; digits++; }
        for (int d = digits - 1; d >= 0; d--) {
            mdbg[i + d] = '0' + (char)(my % 10); my /= 10;
        }
        i += digits;
        mdbg[i] = '\0';
        draw_string(scr, 4, 4, mdbg, 0xFFFFFFFF, rgba(0,0,0,0));
    }
}

/* ── Authentication ───────────────────────────────────────────────────── */
static bool try_login(void)
{
    if (g_fields[FIELD_USER].len < 1) return false;
    int uid = users_authenticate(g_fields[FIELD_USER].buf,
                                  g_fields[FIELD_PASS].buf);
    if (uid < 0) return false;
    strncpy(login_username, g_fields[FIELD_USER].buf, 63);
    login_username[63] = '\0';
    return true;
}

/* ── Main login loop ──────────────────────────────────────────────────── */
void login_run(void)
{
    if (!fb_ready()) return;

    canvas_t screen = draw_main_canvas();

    /* Pre-fill username */
    strncpy(g_fields[FIELD_USER].buf, "user", 63);
    g_fields[FIELD_USER].len    = 4;
    g_fields[FIELD_USER].active = true;
    g_fields[FIELD_PASS].buf[0] = '\0';
    g_fields[FIELD_PASS].len    = 0;
    g_active_field = FIELD_USER;
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
        char ch = keyboard_read();  /* Non-blocking: returns 0 if no key */
        if (ch && ch != (char)prev_kb_char) {
            field_t* f = &g_fields[g_active_field];
            if (ch == '\t') {
                g_active_field = (g_active_field + 1) % FIELD_COUNT;
                g_error = false;
            } else if (ch == '\n' || ch == '\r') {
                if (!prev_enter) {
                    prev_enter = true;
                    if (g_active_field == FIELD_USER) {
                        g_active_field = FIELD_PASS;
                    } else {
                        if (try_login()) return;
                        g_error = true;
                        g_fields[FIELD_PASS].len = 0;
                        g_fields[FIELD_PASS].buf[0] = '\0';
                    }
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

                if (mx >= g_fx && mx < g_fx + FIELD_W &&
                    my >= g_fy1 && my < g_fy1 + FIELD_H) {
                    g_active_field = FIELD_USER;
                    g_error = false;
                } else if (mx >= g_fx && mx < g_fx + FIELD_W &&
                           my >= g_fy2 && my < g_fy2 + FIELD_H) {
                    g_active_field = FIELD_PASS;
                    g_error = false;
                } else if (mx >= g_btn_x && mx < g_btn_x + BTN_W &&
                           my >= g_btn_y && my < g_btn_y + BTN_H) {
                    if (try_login()) return;
                    g_error = true;
                    g_fields[FIELD_PASS].len = 0;
                    g_fields[FIELD_PASS].buf[0] = '\0';
                }
            }
        }

        /* cursor_erase restores back-buf → draw frame → cursor_render
         * writes sprite into back-buf → fb_flip sends it all to VRAM */
        cursor_erase();
        draw_login_screen(&screen);
        cursor_render();
        fb_flip();
        scheduler_yield();
    }
}
