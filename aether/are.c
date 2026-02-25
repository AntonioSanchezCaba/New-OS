/*
 * aether/are.c — Aether Render Engine — Main Loop
 *
 * ARE replaces the old window manager + desktop environment.
 * It is a single-threaded spatial renderer. No X11. No Win32.
 * No taskbar. No windows. Only surfaces, context, and motion.
 *
 * Boot sequence:
 *   kernel_main → are_init() → are_run()
 *   are_run: splash fade → field_init → launch_core_surfaces → loop
 *
 * Input routing:
 *   gesture events  → context_navigate / toggle_overview / launcher
 *   pointer events  → active surface's on_input (if coords overlap)
 *   key events      → active surface's on_input
 */
#include <aether/are.h>
#include <aether/surface.h>
#include <aether/context.h>
#include <aether/field.h>
#include <aether/input.h>
#include <aether/vec.h>
#include <aether/ui.h>
#include <drivers/framebuffer.h>
#include <drivers/timer.h>
#include <drivers/mouse.h>
#include <drivers/cursor.h>
#include <gui/draw.h>
#include <gui/font.h>
#include <kernel/version.h>
#include <gui/theme.h>
#include <services/login.h>
#include <memory.h>
#include <string.h>
#include <kernel.h>
#include <scheduler.h>

/* =========================================================
 * State
 * ========================================================= */
#define STATUS_H  20   /* Status bar height in pixels */

static bool     g_running  = false;
static uint32_t g_screen_w = 0;
static uint32_t g_screen_h = 0;
static uint32_t g_frame    = 0;
static bool     g_logout_requested = false;

/* =========================================================
 * Floating window management
 * ========================================================= */
#define FLOAT_MAX       8
#define FLOAT_TITLE_H   26    /* Height of drag-to-move title bar */
#define FLOAT_BTN_SZ    16    /* Close button size */

static sid_t  g_floats[FLOAT_MAX];
static int    g_float_count = 0;

/* Drag state */
static sid_t  g_drag_sid = SID_NONE;
static vec2_t g_drag_off = { 0, 0 };

static void float_add(sid_t sid)
{
    if (g_float_count < FLOAT_MAX)
        g_floats[g_float_count++] = sid;
}

static void float_remove(sid_t sid)
{
    for (int i = 0; i < g_float_count; i++) {
        if (g_floats[i] == sid) {
            for (int j = i; j < g_float_count - 1; j++)
                g_floats[j] = g_floats[j + 1];
            g_float_count--;
            if (g_drag_sid == sid) g_drag_sid = SID_NONE;
            return;
        }
    }
}

/* =========================================================
 * Status bar  —  drawn over everything, under the cursor
 * Shows: active surface name (left) | uptime + frame count (right)
 * ========================================================= */
static void are_draw_statusbar(canvas_t* c)
{
    int bar_y = (int)c->height - STATUS_H;
    if (bar_y < 0) return;

    /* Background */
    draw_rect(c, 0, bar_y, (int)c->width, STATUS_H,
              ACOLOR(0x0A, 0x12, 0x28, 0xE8));

    /* Separator line at top of bar */
    draw_rect(c, 0, bar_y, (int)c->width, 1,
              ACOLOR(0x30, 0x50, 0xA0, 0xFF));

    /* Left: active surface name */
    sid_t act = context_active();
    const char* name = "AetherOS";
    surface_t* as = surface_get(act);
    if (as && as->title[0]) name = as->title;

    draw_string(c, 8, bar_y + (STATUS_H - FONT_H) / 2,
                name, ACOLOR(0xB0, 0xC8, 0xFF, 0xFF), ACOLOR(0,0,0,0));

    /* Center: OS name */
    const char* center_str = OS_NAME;
    int cx = (int)c->width / 2 - (int)strlen(center_str) * FONT_W / 2;
    draw_string(c, cx, bar_y + (STATUS_H - FONT_H) / 2,
                center_str, ACOLOR(0x50, 0x70, 0xB0, 0xFF), ACOLOR(0,0,0,0));

    /* Right: uptime HH:MM:SS */
    uint32_t total_s  = timer_get_ticks() / TIMER_FREQ;
    uint32_t hh = total_s / 3600;
    uint32_t mm = (total_s % 3600) / 60;
    uint32_t ss = total_s % 60;
    char tbuf[16];
    /* Build string manually (no sprintf in kernel) */
    tbuf[0]  = '0' + (hh / 10) % 10;
    tbuf[1]  = '0' + (hh      ) % 10;
    tbuf[2]  = ':';
    tbuf[3]  = '0' + (mm / 10) % 10;
    tbuf[4]  = '0' + (mm      ) % 10;
    tbuf[5]  = ':';
    tbuf[6]  = '0' + (ss / 10) % 10;
    tbuf[7]  = '0' + (ss      ) % 10;
    tbuf[8]  = '\0';

    int tx = (int)c->width - (int)strlen(tbuf) * FONT_W - 8;
    draw_string(c, tx, bar_y + (STATUS_H - FONT_H) / 2,
                tbuf, ACOLOR(0x80, 0xC0, 0xFF, 0xFF), ACOLOR(0,0,0,0));
}

/* Draw all floating windows over the composed back-buffer */
static void are_compose_floats(canvas_t* c)
{
    for (int fi = 0; fi < g_float_count; fi++) {
        surface_t* f = surface_get(g_floats[fi]);
        if (!f || f->cur_alpha == 0) continue;

        int fx = f->cur_pos.x;
        int fy = f->cur_pos.y;
        int fw = f->cur_w;
        int fh = f->cur_h;  /* body height */

        /* Shadow */
        for (int row = 0; row < fh + FLOAT_TITLE_H && fy + row + 5 < (int)c->height; row++) {
            for (int col = 0; col < fw && fx + col + 6 < (int)c->width; col++) {
                int dx = fx + col + 6, dy = fy + row + 5;
                if (dx < 0 || dy < 0) continue;
                uint32_t* p = &c->pixels[dy * c->width + dx];
                uint32_t bg = *p;
                *p = ACOLOR((uint8_t)(ACOLOR_R(bg)*2/3),
                            (uint8_t)(ACOLOR_G(bg)*2/3),
                            (uint8_t)(ACOLOR_B(bg)*2/3), 0xFF);
            }
        }

        /* Title bar background */
        uint32_t bar_col = ACOLOR(0x1A, 0x2E, 0x5A, 0xFF);
        draw_rect(c, fx, fy, fw, FLOAT_TITLE_H, bar_col);

        /* Close button (top-right X) */
        int bx = fx + fw - FLOAT_BTN_SZ - 4;
        int by = fy + (FLOAT_TITLE_H - FLOAT_BTN_SZ) / 2;
        draw_rect(c, bx, by, FLOAT_BTN_SZ, FLOAT_BTN_SZ, ACOLOR(0xC0,0x30,0x30,0xFF));
        draw_string(c, bx + 4, by + 3, "x", 0xFFFFFFFF, ACOLOR(0,0,0,0));

        /* Title text */
        draw_string(c, fx + 8, fy + (FLOAT_TITLE_H - FONT_H) / 2,
                    f->title, 0xFFFFFFFF, ACOLOR(0,0,0,0));

        /* Window border */
        draw_rect_outline(c, fx, fy, fw, fh + FLOAT_TITLE_H, 1,
                          ACOLOR(0x40, 0x70, 0xD0, 0xFF));

        /* Body surface */
        are_blit_surface(c->pixels, (int)c->width, (int)c->height,
                         f->buf, (int)f->buf_w, (int)f->buf_h,
                         fx, fy + FLOAT_TITLE_H,
                         fw, fh, f->cur_alpha);
    }
}

/* Forward declarations */
static void are_dispatch_input(void);
static void are_splash(void);
static void are_launch_core_surfaces(void);

/* =========================================================
 * Queries
 * ========================================================= */
uint32_t are_screen_w(void)   { return g_screen_w; }
uint32_t are_screen_h(void)   { return g_screen_h; }
bool     are_running(void)    { return g_running;  }
void     are_shutdown(void)   { g_running = false; }
void     are_logout(void)     { g_logout_requested = true; g_running = false; }

/* =========================================================
 * Init
 * ========================================================= */
void are_init(void)
{
    g_screen_w = fb.width;
    g_screen_h = fb.height;

    mouse_set_bounds((int)g_screen_w - 1, (int)g_screen_h - 1);

    surface_init();
    context_init();
    input_init();
    ui_pool_reset();
    theme_init();   /* ensure colour theme is active in ARE mode */

    kinfo("ARE v%d.%d initialized — %ux%u",
          ARE_VERSION_MAJOR, ARE_VERSION_MINOR, g_screen_w, g_screen_h);
}

/* =========================================================
 * Surface allocation helper
 * ========================================================= */
sid_t are_add_surface(surface_type_t type,
                       uint32_t w, uint32_t h,
                       const char* title, const char* icon,
                       surface_render_fn rfn, surface_input_fn ifn,
                       surface_event_fn cfn, void* ud)
{
    sid_t sid = surface_create(type, w, h, title, icon, rfn, ifn, cfn, ud);
    if (sid == SID_NONE) return SID_NONE;

    if (type == SURF_OVERLAY) {
        context_push_overlay(sid);
    } else if (type == SURF_FLOAT) {
        /* Position centered on screen by default; full-alpha, no animation */
        surface_t* s = surface_get(sid);
        if (s) {
            s->cur_pos.x = (int)(g_screen_w > s->buf_w ? (g_screen_w - s->buf_w) / 2 : 0);
            s->cur_pos.y = (int)(g_screen_h > s->buf_h + FLOAT_TITLE_H
                                 ? (g_screen_h - s->buf_h - FLOAT_TITLE_H) / 2 : 40);
            s->tgt_pos   = s->cur_pos;
            s->cur_w     = (int)s->buf_w;
            s->tgt_w     = s->cur_w;
            s->cur_h     = (int)s->buf_h;   /* body height only; title drawn separately */
            s->tgt_h     = s->cur_h;
            s->cur_alpha = 255;
            s->tgt_alpha = 255;
        }
        float_add(sid);
    } else {
        context_add_surface(sid);
    }

    surface_invalidate(sid);
    return sid;
}

void are_remove_surface(sid_t id)
{
    float_remove(id);           /* no-op if not a float */
    context_remove_surface(id);
    surface_destroy(id);
}

void are_push_overlay(sid_t id) { context_push_overlay(id); }
void are_pop_overlay(void)      { context_pop_overlay();    }

sid_t are_add_float(uint32_t w, uint32_t h, const char* title,
                    surface_render_fn rfn, surface_input_fn ifn, void* ud)
{
    return are_add_surface(SURF_FLOAT, w, h, title, "W", rfn, ifn, NULL, ud);
}

/* =========================================================
 * Alpha-blending blit with nearest-neighbour scale
 * ========================================================= */
void are_blit_surface(uint32_t* dst, int dw, int dh,
                       const uint32_t* src, int sw, int sh,
                       int x, int y, int bw, int bh, uint8_t alpha)
{
    if (!dst || !src || bw <= 0 || bh <= 0 || alpha == 0) return;

    for (int row = 0; row < bh; row++) {
        int dy = y + row;
        if (dy < 0 || dy >= dh) continue;
        int sy = (row * sh) / bh;
        if (sy >= sh) continue;

        for (int col = 0; col < bw; col++) {
            int dx = x + col;
            if (dx < 0 || dx >= dw) continue;
            int sx = (col * sw) / bw;
            if (sx >= sw) continue;

            uint32_t sp = src[sy * sw + sx];
            /* Combine source alpha with compositing alpha */
            uint8_t sa = (uint8_t)(((uint32_t)ACOLOR_A(sp) * alpha) / 255);
            if (sa == 0) continue;

            uint32_t dp = dst[dy * dw + dx];
            uint8_t ia = 255 - sa;
            uint8_t r = (uint8_t)((ACOLOR_R(sp)*sa + ACOLOR_R(dp)*ia) / 255);
            uint8_t g = (uint8_t)((ACOLOR_G(sp)*sa + ACOLOR_G(dp)*ia) / 255);
            uint8_t b = (uint8_t)((ACOLOR_B(sp)*sa + ACOLOR_B(dp)*ia) / 255);
            dst[dy * dw + dx] = (0xFFu<<24)|(r<<16)|(g<<8)|b;
        }
    }
}

/* =========================================================
 * Input dispatch
 * ========================================================= */
static void are_dispatch_input(void)
{
    input_event_t ev;
    while (input_pop(&ev)) {
        if (ev.type == INPUT_GESTURE) {
            /* Navigation gestures: handled by ARE, not surfaces */
            switch (ev.gesture.type) {
            case GESTURE_SWIPE_LEFT:
                context_navigate(+1);
                break;
            case GESTURE_SWIPE_RIGHT:
                context_navigate(-1);
                break;
            case GESTURE_OVERVIEW:
                context_toggle_overview();
                break;
            case GESTURE_FOCUS:
                if (g_ctx.overview) context_toggle_overview();
                break;
            case GESTURE_LAUNCHER:
                /* Toggle launcher overlay */
                if (context_overlay_visible())
                    context_pop_overlay();
                else {
                    /* Find launcher surface (type SURF_OVERLAY) */
                    for (int i = 0; i < SURFACE_MAX; i++) {
                        surface_t* s = surface_get((sid_t)i);
                        if (s && s->type == SURF_OVERLAY) {
                            context_push_overlay(s->id);
                            break;
                        }
                    }
                }
                break;
            case GESTURE_CLOSE_SURFACE:
                /* Animate out active surface */
                if (g_ctx.field_count > 0) {
                    sid_t act = context_active();
                    are_remove_surface(act);
                }
                break;
            default: break;
            }
            continue;  /* never forward gesture to surfaces */
        }

        if (g_ctx.overview) {
            /* In overview mode: pointer clicks select a surface */
            if (ev.type == INPUT_POINTER &&
                (ev.pointer.buttons & IBTN_LEFT) &&
                !(ev.pointer.prev_buttons & IBTN_LEFT)) {
                int hit = field_overview_hit(ev.pointer.x, ev.pointer.y,
                                              g_screen_w, g_screen_h);
                if (hit >= 0) {
                    context_toggle_overview();
                    context_goto(hit);
                }
            }
            /* Update hover */
            if (ev.type == INPUT_POINTER)
                g_ctx.overview_hover = field_overview_hit(
                    ev.pointer.x, ev.pointer.y, g_screen_w, g_screen_h);
            continue;
        }

        /* Overlay surfaces get input first */
        if (context_overlay_visible() && g_ctx.overlay_count > 0) {
            surface_t* ov = surface_get(
                g_ctx.overlays[g_ctx.overlay_count - 1]);
            if (ov && ov->on_input)
                ov->on_input(ov->id, &ev, ov->userdata);
            continue;
        }

        /* Floating windows: drag + hit-test (back-to-front order) */
        if (g_float_count > 0 && ev.type == INPUT_POINTER) {
            bool float_hit = false;
            /* Active drag takes priority */
            if (g_drag_sid != SID_NONE) {
                surface_t* df = surface_get(g_drag_sid);
                if (df) {
                    bool btn_up = !(ev.pointer.buttons & IBTN_LEFT)
                                && (ev.pointer.prev_buttons & IBTN_LEFT);
                    if (btn_up) {
                        g_drag_sid = SID_NONE;
                    } else {
                        df->cur_pos.x = ev.pointer.x + g_drag_off.x;
                        df->cur_pos.y = ev.pointer.y + g_drag_off.y;
                        df->tgt_pos   = df->cur_pos;
                    }
                    float_hit = true;
                }
            }
            /* New hit-test (top float first) */
            if (!float_hit) {
                for (int fi = g_float_count - 1; fi >= 0; fi--) {
                    surface_t* f = surface_get(g_floats[fi]);
                    if (!f) continue;
                    int fx = f->cur_pos.x, fy = f->cur_pos.y;
                    int fw = f->cur_w,    fh = f->cur_h + FLOAT_TITLE_H;
                    int mx = ev.pointer.x, my = ev.pointer.y;
                    if (mx < fx || mx >= fx+fw || my < fy || my >= fy+fh) continue;

                    bool btn_dn = (ev.pointer.buttons & IBTN_LEFT)
                               && !(ev.pointer.prev_buttons & IBTN_LEFT);
                    bool in_titlebar = (my < fy + FLOAT_TITLE_H);

                    /* Close-button click */
                    int bx = fx + fw - FLOAT_BTN_SZ - 4;
                    int by = fy + (FLOAT_TITLE_H - FLOAT_BTN_SZ) / 2;
                    if (btn_dn && mx >= bx && mx < bx+FLOAT_BTN_SZ
                               && my >= by && my < by+FLOAT_BTN_SZ) {
                        are_remove_surface(g_floats[fi]);
                        float_hit = true;
                        break;
                    }
                    if (btn_dn && in_titlebar) {
                        g_drag_sid   = f->id;
                        g_drag_off.x = fx - mx;
                        g_drag_off.y = fy - my;
                        float_hit = true;
                        break;
                    }
                    /* Body click → forward translated coords */
                    if (f->on_input && !in_titlebar) {
                        input_event_t lev = ev;
                        lev.pointer.x -= fx;
                        lev.pointer.y -= fy + FLOAT_TITLE_H;
                        f->on_input(f->id, &lev, f->userdata);
                    }
                    float_hit = true;
                    break;
                }
            }
            if (float_hit) continue;
        }

        /* Forward to active surface */
        sid_t act = context_active();
        surface_t* s = surface_get(act);
        if (s && s->on_input)
            s->on_input(act, &ev, s->userdata);
    }
}

/* =========================================================
 * Splash (fade-in from black to Field)
 * ========================================================= */
static void are_splash(void)
{
    canvas_t sc = draw_main_canvas();
    uint32_t sw = sc.width, sh = sc.height;

    /* Quick 30-frame fade from black with centered title */
    for (int f = 0; f <= 30; f++) {
        uint8_t a = (uint8_t)((f * 255) / 30);

        /* Black background */
        for (int i = 0; i < (int)(sw * sh); i++)
            sc.pixels[i] = 0xFF000000;

        /* Fade in the ARE name + OS version */
        uint32_t tc = ACOLOR(0xA0, 0xC0, 0xFF, a);
        uint32_t sc2= ACOLOR(0x60, 0x80, 0xB0, a);

        int title_x = (int)sw / 2 - (int)strlen(OS_NAME)  * FONT_W / 2;
        int title_y = (int)sh / 2 - FONT_H;
        int ver_x   = (int)sw / 2 - (int)strlen(OS_BANNER_SHORT) * FONT_W / 2;
        int ver_y   = title_y + FONT_H + 6;

        draw_string(&sc, title_x, title_y, OS_NAME,         tc,  ACOLOR(0,0,0,0));
        draw_string(&sc, ver_x,   ver_y,   OS_BANNER_SHORT,  sc2, ACOLOR(0,0,0,0));

        fb_flip();

        /* Small delay */
        uint32_t t0 = timer_get_ticks();
        while (timer_get_ticks() - t0 < 2) scheduler_yield();
    }
}

/* =========================================================
 * Launch core surfaces
 * ========================================================= */

/* Forward declarations from surface modules */
extern void surface_terminal_init(uint32_t w, uint32_t h);
extern void surface_explorer_init(uint32_t w, uint32_t h);
extern void surface_sysmon_init(uint32_t w, uint32_t h);
extern void surface_settings_init(uint32_t w, uint32_t h);
extern void surface_launcher_init(uint32_t w, uint32_t h);

static void are_launch_core_surfaces(void)
{
    /* Surface dimensions: nearly full screen */
    uint32_t sw = g_screen_w - 2 * FIELD_SURF_MARGIN_X;
    uint32_t sh = g_screen_h - FIELD_SURF_MARGIN_TOP - FIELD_SURF_MARGIN_BOT;

    surface_terminal_init(sw, sh);
    surface_explorer_init(sw, sh);
    surface_sysmon_init(sw, sh);
    surface_settings_init(sw, sh);

    /* Launcher overlay (smaller, centered) */
    uint32_t lw = sw * 3 / 4;
    uint32_t lh = sh * 3 / 4;
    surface_launcher_init(lw, lh);

    kinfo("ARE: %d surfaces registered", g_ctx.field_count);
}

/* =========================================================
 * Main render loop
 * ========================================================= */
void are_run(void)
{
    if (!fb_ready()) {
        klog_warn("ARE: framebuffer not ready, aborting");
        return;
    }

    field_init(g_screen_w, g_screen_h);
    are_splash();

    do {
        g_logout_requested = false;

        /* Destroy all surfaces from the previous session (no-op first time) */
        for (int i = 0; i < g_ctx.field_count; i++)
            surface_destroy(g_ctx.field[i]);
        for (int i = 0; i < g_ctx.overlay_count; i++)
            surface_destroy(g_ctx.overlays[i]);
        for (int i = 0; i < g_float_count; i++)
            surface_destroy(g_floats[i]);
        g_float_count = 0;
        g_drag_sid    = SID_NONE;
        surface_init();
        context_init();

        login_run();              /* graphical login screen — blocks until authenticated */
        are_launch_core_surfaces();

        g_running = true;
        uint32_t last_ticks = 0;
        canvas_t screen;

        kinfo("ARE: entering render loop");

        while (g_running) {
            uint32_t now = timer_get_ticks();
            /* ~60 Hz throttle */
            if (now - last_ticks < 2) { scheduler_yield(); continue; }
            last_ticks = now;
            g_frame++;

            /* 1. Poll hardware input */
            input_poll();

            /* 2. Dispatch input → gestures / surfaces */
            are_dispatch_input();

            /* 3. Tick animations */
            context_tick();
            field_tick();

            /* 4. Render dirty surfaces to their own buffers */
            for (int i = 0; i < g_ctx.field_count; i++)
                surface_tick(g_ctx.field[i]);
            for (int i = 0; i < g_ctx.overlay_count; i++)
                surface_tick(g_ctx.overlays[i]);
            for (int i = 0; i < g_float_count; i++)
                surface_tick(g_floats[i]);

            /* 5. Compose to back-buffer */
            screen = draw_main_canvas();
            field_draw_background(&screen);

            if (g_ctx.overview) {
                field_compose(&screen);
                field_draw_overview(&screen);
            } else {
                field_compose(&screen);
            }

            field_draw_nav_dots(&screen);

            /* 5b. Compose floating windows above field/overlays */
            are_compose_floats(&screen);

            /* 5c. Draw status bar (below cursor, above everything else) */
            are_draw_statusbar(&screen);

            /* 6. Render software cursor over the composed frame */
            cursor_erase();   /* restore pixels under old cursor position */
            cursor_render();  /* save pixels, blit cursor sprite */

            /* 7. Flip */
            fb_flip();

            scheduler_yield();
        }

        kinfo("ARE: session ended%s", g_logout_requested ? " (logout)" : "");
    } while (g_logout_requested);

    kinfo("ARE: render loop exited");
}
