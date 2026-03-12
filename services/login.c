/*
 * services/login.c - Aether OS graphical login screen
 *
 * Renders a full-screen login UI with username + password fields.
 * For v0.1, any non-empty credentials are accepted.
 */
#include <services/login.h>
#include <kernel/version.h>
#include <gui/draw.h>
#include <gui/theme.h>
#include <drivers/framebuffer.h>
#include <drivers/keyboard.h>
#include <drivers/timer.h>
#include <drivers/mouse.h>
#include <scheduler.h>
#include <kernel/users.h>
#include <string.h>
#include <memory.h>

char login_username[64] = "user";

#define BOX_W  380
#define BOX_H  300
#define FIELD_W 300
#define FIELD_H 32
#define FIELD_PAD 8

#define FIELD_USER 0
#define FIELD_PASS 1
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

static void draw_field(canvas_t* screen, int x, int y, int w, int h,
                       const char* label, field_t* f, bool is_password,
                       bool active)
{
    const theme_t* th = theme_current();

    /* Label */
    draw_string(screen, x, y - FONT_H - 4, label, th->login_text, rgba(0,0,0,0));

    /* Field background */
    uint32_t bg  = th->login_field_bg;
    uint32_t brd = active ? th->login_cursor : th->login_field_border;
    draw_rect(screen, x, y, w, h, bg);
    draw_rect_outline(screen, x, y, w, h, active ? 2 : 1, brd);

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
    draw_string(screen, tx, ty, display, th->login_text, rgba(0,0,0,0));

    /* Blinking cursor */
    if (active && g_cursor_vis) {
        int cw = (int)strlen(display) * FONT_W;
        draw_rect(screen, tx + cw + 1, ty, 2, FONT_H, th->login_cursor);
    }
}

static void draw_login_screen(canvas_t* screen)
{
    const theme_t* th = theme_current();
    int W = screen->width;
    int H = screen->height;
    int cx = W / 2;
    int cy = H / 2;

    /* Full-screen background */
    draw_gradient_v(screen, 0, 0, W, H, th->login_bg, th->login_bg + 0x00060606u);

    /* Subtle background pattern */
    for (int y = 0; y < H; y += 50)
        draw_hline(screen, 0, y, W, rgba(0x40, 0x70, 0xA0, 0x08));
    for (int x = 0; x < W; x += 50)
        draw_vline(screen, x, 0, H, rgba(0x40, 0x70, 0xA0, 0x08));

    /* Login box */
    int bx = cx - BOX_W / 2;
    int by = cy - BOX_H / 2;
    draw_rect(screen, bx, by, BOX_W, BOX_H, th->login_box);
    draw_rect_outline(screen, bx, by, BOX_W, BOX_H, 1, th->login_box_border);

    /* OS logo above box */
    const char* logo = OS_NAME;
    int lw = (int)strlen(logo) * FONT_W;
    draw_string(screen, cx - lw / 2, by - FONT_H * 2 - 8, logo,
                th->accent, rgba(0,0,0,0));
    const char* sub = OS_BANNER_SHORT;
    int sw = (int)strlen(sub) * FONT_W;
    draw_string(screen, cx - sw / 2, by - FONT_H - 4, sub,
                th->splash_text, rgba(0,0,0,0));

    /* Box header */
    int hdr_h = 40;
    draw_rect(screen, bx, by, BOX_W, hdr_h, th->win_title_bg);
    draw_rect_outline(screen, bx, by, BOX_W, hdr_h, 0, 0);
    draw_hline(screen, bx, by + hdr_h, BOX_W, th->win_border);
    const char* hdr = "Sign in to " OS_NAME;
    int hw = (int)strlen(hdr) * FONT_W;
    draw_string(screen, cx - hw / 2, by + (hdr_h - FONT_H) / 2, hdr,
                th->win_title_text, rgba(0,0,0,0));

    /* User icon placeholder */
    int icon_cx = cx;
    int icon_cy = by + hdr_h + 44;
    draw_circle_filled(screen, icon_cx, icon_cy, 22, th->accent);
    draw_circle(screen, icon_cx, icon_cy, 22, th->accent2);
    /* Simple face */
    draw_circle_filled(screen, icon_cx, icon_cy - 3, 10, th->login_box);
    draw_circle_filled(screen, icon_cx - 2, icon_cy + 8, 8, th->login_box);
    draw_circle_filled(screen, icon_cx + 2, icon_cy + 8, 8, th->login_box);

    /* Username field */
    int fy1 = by + hdr_h + 80;
    int fx  = cx - FIELD_W / 2;
    draw_field(screen, fx, fy1, FIELD_W, FIELD_H, "Username",
               &g_fields[FIELD_USER], false, g_active_field == FIELD_USER);

    /* Password field */
    int fy2 = fy1 + FIELD_H + 30;
    draw_field(screen, fx, fy2, FIELD_W, FIELD_H, "Password",
               &g_fields[FIELD_PASS], true, g_active_field == FIELD_PASS);

    /* Error message */
    if (g_error) {
        const char* err = "Invalid credentials. Please try again.";
        int ew = (int)strlen(err) * FONT_W;
        draw_string(screen, cx - ew / 2, fy2 + FIELD_H + 8, err,
                    th->error, rgba(0,0,0,0));
    }

    /* Login button */
    int btn_y  = fy2 + FIELD_H + (g_error ? 30 : 16);
    int btn_w  = 120;
    int btn_h  = 34;
    int btn_x  = cx - btn_w / 2;
    draw_rect_rounded(screen, btn_x, btn_y, btn_w, btn_h, 4, th->accent);
    const char* btn_lbl = "Sign In";
    int blw = (int)strlen(btn_lbl) * FONT_W;
    draw_string(screen, cx - blw / 2, btn_y + (btn_h - FONT_H) / 2,
                btn_lbl, th->text_on_accent, rgba(0,0,0,0));

    /* Keyboard hint */
    const char* hint = "Press Enter to sign in";
    int hiw = (int)strlen(hint) * FONT_W;
    draw_string(screen, cx - hiw / 2, by + BOX_H - FONT_H - 8, hint,
                th->text_secondary, rgba(0,0,0,0));

    /* Clock at bottom right */
    uint32_t secs = timer_get_ticks() / TIMER_FREQ;
    char clock_str[16];
    uint32_t h2 = (secs / 3600) % 24;
    uint32_t m2 = (secs / 60)   % 60;
    uint32_t s2 =  secs         % 60;
    clock_str[0] = '0' + (char)(h2/10); clock_str[1] = '0' + (char)(h2%10);
    clock_str[2] = ':';
    clock_str[3] = '0' + (char)(m2/10); clock_str[4] = '0' + (char)(m2%10);
    clock_str[5] = ':';
    clock_str[6] = '0' + (char)(s2/10); clock_str[7] = '0' + (char)(s2%10);
    clock_str[8] = '\0';
    draw_string(screen, W - 9 * FONT_W - 8, H - FONT_H - 8,
                clock_str, th->splash_text, rgba(0,0,0,0));
}

static bool try_login(void)
{
    if (g_fields[FIELD_USER].len < 1) return false;
    /* Authenticate against the user database */
    int uid = users_authenticate(g_fields[FIELD_USER].buf,
                                  g_fields[FIELD_PASS].buf);
    if (uid < 0) return false;
    strncpy(login_username, g_fields[FIELD_USER].buf, 63);
    login_username[63] = '\0';
    return true;
}

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

    /* Diagnostic: paint a red-and-white stripe pattern for ~1 second to
     * confirm that login_run() is running AND draw_rect+fb_flip reach VRAM.
     * If the user sees alternating stripes, login_run() works; the bug is
     * purely cosmetic inside draw_login_screen().  Remove once confirmed. */
    {
        uint32_t stripe_h = 30;
        for (uint32_t row = 0; row < (uint32_t)screen.height; row++) {
            uint32_t col = (row / stripe_h) % 2 ? 0xFFFFFFFFu : 0xFFFF0000u;
            for (uint32_t x = 0; x < (uint32_t)screen.width; x++)
                screen.pixels[row * (uint32_t)screen.stride + x] = col;
        }
        fb_flip();
        uint32_t _t = timer_get_ticks();
        while (timer_get_ticks() - _t < 100) {}  /* hold ~1 second */
    }

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
                /* Tab: switch field */
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
                        /* Clear password field */
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

        /* --- Mouse click: field focus or login button --- */
        /* (Simplified: just use keyboard for login) */

        /* Render */
        draw_login_screen(&screen);
        fb_flip();
        scheduler_yield();
    }
}
