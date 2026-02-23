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
#include <memory.h>
#include <string.h>
#include <kernel.h>
#include <scheduler.h>

/* =========================================================
 * State
 * ========================================================= */
static bool     g_running  = false;
static uint32_t g_screen_w = 0;
static uint32_t g_screen_h = 0;
static uint32_t g_frame    = 0;

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

    if (type == SURF_OVERLAY)
        context_push_overlay(sid);
    else
        context_add_surface(sid);

    surface_invalidate(sid);
    return sid;
}

void are_remove_surface(sid_t id)
{
    context_remove_surface(id);
    surface_destroy(id);
}

void are_push_overlay(sid_t id) { context_push_overlay(id); }
void are_pop_overlay(void)      { context_pop_overlay();    }

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

        /* 6. Render software cursor over the composed frame */
        cursor_erase();   /* restore pixels under old cursor position */
        cursor_render();  /* save pixels, blit cursor sprite */

        /* 7. Flip */
        fb_flip();

        scheduler_yield();
    }

    kinfo("ARE: render loop exited");
}
