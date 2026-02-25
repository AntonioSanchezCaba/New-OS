/*
 * gui/window_manager.c - Window manager and compositor
 *
 * Manages a stack of up to WM_MAX_WINDOWS windows.
 * Each window owns its pixel buffer (titlebar + client area).
 * The compositor walks the z-order array (back-to-front) and blits
 * each window's buffer to the screen canvas, preceded by a drop shadow.
 *
 * Resize handles: windows with WF_RESIZEABLE expose WM_RESIZE_ZONE pixel
 * hot-zones at the right/bottom/corner edges for mouse-drag resizing.
 */
#include <gui/window.h>
#include <gui/event.h>
#include <gui/draw.h>
#include <drivers/framebuffer.h>
#include <drivers/mouse.h>
#include <memory.h>
#include <string.h>
#include <kernel.h>

/* =========================================================
 * State
 * ========================================================= */

static window_t windows[WM_MAX_WINDOWS];
static bool     window_used[WM_MAX_WINDOWS];

/*
 * z_order[0] = back-most window index into windows[]
 * z_order[n-1] = front-most (topmost) window
 */
static int z_order[WM_MAX_WINDOWS];
static int z_count = 0;          /* Number of valid entries */

wid_t wm_focused_wid = -1;

static int    next_wid = 1;      /* Auto-incrementing window ID */

/* =========================================================
 * Internal helpers
 * ========================================================= */

static int wm_find_slot(wid_t wid)
{
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        if (window_used[i] && windows[i].id == wid)
            return i;
    }
    return -1;
}

static int wm_alloc_slot(void)
{
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        if (!window_used[i]) return i;
    }
    return -1;
}

/* Remove from z_order by slot index */
static void z_remove(int slot)
{
    for (int i = 0; i < z_count; i++) {
        if (z_order[i] == slot) {
            for (int j = i; j < z_count - 1; j++)
                z_order[j] = z_order[j + 1];
            z_count--;
            return;
        }
    }
}

/* Push slot to top (front) of z_order */
static void z_push_front(int slot)
{
    z_remove(slot);
    if (z_count < WM_MAX_WINDOWS)
        z_order[z_count++] = slot;
}

/* Draw a window's titlebar content into its own buffer */
static void draw_titlebar(window_t* w)
{
    /* Gradient background based on focus */
    uint32_t col_a = (w->flags & WF_FOCUSED) ? COLOR_WIN_TITLE : COLOR_MID_GREY;
    uint32_t col_b = (w->flags & WF_FOCUSED)
                     ? rgb(0x20, 0x40, 0x70) : COLOR_DARK_GREY;

    /* Create a canvas over the full window buffer */
    canvas_t full = {
        .pixels = w->buf,
        .width  = w->w,
        .height = WM_TITLEBAR_H + w->h,
        .stride = w->w
    };

    /* Gradient in titlebar */
    draw_gradient_v(&full, 0, 0, w->w, WM_TITLEBAR_H, col_a, col_b);

    /* Title text */
    draw_string_centered(&full, 0, 0, w->w - 26, WM_TITLEBAR_H,
                         w->title, COLOR_TEXT_LIGHT, rgba(0,0,0,0));

    /* Close button (red X in top-right) */
    if (w->flags & WF_CLOSEABLE) {
        int bx = w->w - 22;
        int by = 4;
        draw_rect_rounded(&full, bx, by, 18, 18, 4, COLOR_BTN_CLOSE);
        /* X marks */
        draw_string(&full, bx + 5, by + 3, "x", COLOR_TEXT_LIGHT, rgba(0,0,0,0));
    }

    /* Bottom separator line */
    draw_hline(&full, 0, WM_TITLEBAR_H - 1, w->w, COLOR_WIN_BORDER);
}

/* =========================================================
 * WM public API
 * ========================================================= */

void wm_init(void)
{
    memset(windows, 0, sizeof(windows));
    memset(window_used, 0, sizeof(window_used));
    memset(z_order, 0, sizeof(z_order));
    z_count = 0;
    wm_focused_wid = -1;
}

wid_t wm_create_window(const char* title, int x, int y, int w, int h,
                        window_event_fn cb, void* userdata)
{
    int slot = wm_alloc_slot();
    if (slot < 0) {
        klog_warn("wm: no free window slots");
        return -1;
    }

    size_t buf_size = (size_t)w * (size_t)(WM_TITLEBAR_H + h) * sizeof(uint32_t);
    uint32_t* buf = (uint32_t*)kmalloc(buf_size);
    if (!buf) {
        klog_warn("wm: kmalloc failed for window buffer");
        return -1;
    }
    memset(buf, 0, buf_size);

    window_t* win = &windows[slot];
    win->id        = next_wid++;
    win->x         = x;
    win->y         = y;
    win->w         = w;
    win->h         = h;
    win->flags     = WF_VISIBLE | WF_MOVEABLE | WF_CLOSEABLE | WF_RESIZEABLE;
    win->buf       = buf;
    win->on_event  = cb;
    win->userdata  = userdata;
    win->drag_active = false;

    /* Client canvas starts right after the titlebar rows */
    win->canvas.pixels = buf + (size_t)WM_TITLEBAR_H * (size_t)w;
    win->canvas.width  = w;
    win->canvas.height = h;
    win->canvas.stride = w;

    strncpy(win->title, title, sizeof(win->title) - 1);
    win->title[sizeof(win->title) - 1] = '\0';

    window_used[slot] = true;
    z_push_front(slot);
    wm_focus(win->id);

    /* Initial paint */
    draw_titlebar(win);
    draw_rect(&win->canvas, 0, 0, w, h, COLOR_WIN_BG);

    if (cb) {
        gui_event_t paint_evt = { .type = GUI_EVENT_PAINT };
        cb(win->id, &paint_evt, userdata);
    }

    return win->id;
}

void wm_destroy_window(wid_t wid)
{
    int slot = wm_find_slot(wid);
    if (slot < 0) return;

    window_t* w = &windows[slot];
    if (w->buf) { kfree(w->buf); w->buf = NULL; }
    z_remove(slot);
    window_used[slot] = false;

    if (wm_focused_wid == wid) {
        wm_focused_wid = (z_count > 0) ? windows[z_order[z_count-1]].id : -1;
    }
}

window_t* wm_get_window(wid_t wid)
{
    int slot = wm_find_slot(wid);
    return (slot >= 0) ? &windows[slot] : NULL;
}

void wm_show(wid_t wid)
{
    window_t* w = wm_get_window(wid);
    if (w) w->flags |= WF_VISIBLE;
}

void wm_hide(wid_t wid)
{
    window_t* w = wm_get_window(wid);
    if (w) w->flags &= ~WF_VISIBLE;
}

void wm_focus(wid_t wid)
{
    /* Unfocus previous */
    if (wm_focused_wid >= 0) {
        window_t* prev = wm_get_window(wm_focused_wid);
        if (prev) {
            prev->flags &= ~WF_FOCUSED;
            draw_titlebar(prev);
        }
    }
    wm_focused_wid = wid;
    window_t* w = wm_get_window(wid);
    if (w) {
        w->flags |= WF_FOCUSED;
        draw_titlebar(w);
    }
}

void wm_move(wid_t wid, int x, int y)
{
    window_t* w = wm_get_window(wid);
    if (!w) return;
    /* Clamp so titlebar stays visible */
    if (y < 0) y = 0;
    w->x = x;
    w->y = y;
}

void wm_resize(wid_t wid, int new_w, int new_h)
{
    window_t* w = wm_get_window(wid);
    if (!w || new_w <= 0 || new_h <= 0) return;

    size_t buf_size = (size_t)new_w * (size_t)(WM_TITLEBAR_H + new_h) * sizeof(uint32_t);
    uint32_t* new_buf = (uint32_t*)kmalloc(buf_size);
    if (!new_buf) return;
    memset(new_buf, 0, buf_size);

    kfree(w->buf);
    w->buf = new_buf;
    w->w   = new_w;
    w->h   = new_h;
    w->canvas.pixels = new_buf + (size_t)WM_TITLEBAR_H * (size_t)new_w;
    w->canvas.width  = new_w;
    w->canvas.height = new_h;
    w->canvas.stride = new_w;

    draw_titlebar(w);
    draw_rect(&w->canvas, 0, 0, new_w, new_h, COLOR_WIN_BG);

    if (w->on_event) {
        gui_event_t evt = { .type = GUI_EVENT_RESIZE,
                            .resize = { .w = new_w, .h = new_h } };
        w->on_event(wid, &evt, w->userdata);
    }
}

void wm_raise(wid_t wid)
{
    int slot = wm_find_slot(wid);
    if (slot >= 0) z_push_front(slot);
}

void wm_close(wid_t wid)
{
    window_t* w = wm_get_window(wid);
    if (w && w->on_event) {
        gui_event_t evt = { .type = GUI_EVENT_CLOSE };
        w->on_event(wid, &evt, w->userdata);
    }
    wm_destroy_window(wid);
}

void wm_invalidate(wid_t wid)
{
    window_t* w = wm_get_window(wid);
    if (!w || !w->on_event) return;
    gui_event_t evt = { .type = GUI_EVENT_PAINT };
    w->on_event(wid, &evt, w->userdata);
}

void wm_invalidate_all(void)
{
    for (int i = 0; i < z_count; i++) {
        if (window_used[z_order[i]])
            wm_invalidate(windows[z_order[i]].id);
    }
}

canvas_t wm_client_canvas(wid_t wid)
{
    window_t* w = wm_get_window(wid);
    if (!w) {
        canvas_t empty = {NULL, 0, 0, 0};
        return empty;
    }
    return w->canvas;
}

/* =========================================================
 * Event dispatch (called each frame by GUI main loop)
 * ========================================================= */

void wm_dispatch_events(void)
{
    gui_event_t evt;
    while (gui_event_pop(&evt)) {
        if (evt.type == GUI_EVENT_MOUSE_MOVE   ||
            evt.type == GUI_EVENT_MOUSE_DOWN   ||
            evt.type == GUI_EVENT_MOUSE_UP) {

            int mx = evt.mouse.x;
            int my = evt.mouse.y;

            /* Hit-test windows front-to-back */
            int hit_slot = -1;
            for (int i = z_count - 1; i >= 0; i--) {
                int s = z_order[i];
                if (!window_used[s]) continue;
                window_t* w = &windows[s];
                if (!(w->flags & WF_VISIBLE)) continue;

                int wx = w->x;
                int wy = w->y;
                int ww = w->w;
                int wh = WM_TITLEBAR_H + w->h + WM_BORDER_W * 2;
                int full_x0 = wx - WM_BORDER_W;
                int full_y0 = wy - WM_BORDER_W;
                int full_x1 = wx + ww + WM_BORDER_W;
                int full_y1 = wy + wh + WM_BORDER_W;

                if (mx >= full_x0 && mx < full_x1 &&
                    my >= full_y0 && my < full_y1) {
                    hit_slot = s;
                    break;
                }
            }

            if (evt.type == GUI_EVENT_MOUSE_DOWN && hit_slot >= 0) {
                window_t* w = &windows[hit_slot];
                wm_focus(w->id);
                wm_raise(w->id);

                int wx = w->x, wy = w->y;
                int win_total_h = WM_TITLEBAR_H + w->h;

                /* Close button hit? */
                if ((w->flags & WF_CLOSEABLE) &&
                    mx >= wx + w->w - 22 && mx < wx + w->w - 4 &&
                    my >= wy + 4 && my < wy + 22) {
                    wm_close(w->id);
                    continue;
                }

                /* Resize handle hit? (right/bottom edges + corners) */
                if (w->flags & WF_RESIZEABLE) {
                    bool on_right  = (mx >= wx + w->w - WM_RESIZE_ZONE) &&
                                     (mx <  wx + w->w + WM_BORDER_W);
                    bool on_bottom = (my >= wy + win_total_h - WM_RESIZE_ZONE) &&
                                     (my <  wy + win_total_h + WM_BORDER_W);

                    if (on_right || on_bottom) {
                        w->resize_active  = true;
                        w->resize_edge    = (on_right  ? RESIZE_RIGHT  : 0) |
                                            (on_bottom ? RESIZE_BOTTOM : 0);
                        w->resize_start_mx = mx;
                        w->resize_start_my = my;
                        w->resize_start_x  = wx;
                        w->resize_start_y  = wy;
                        w->resize_start_w  = w->w;
                        w->resize_start_h  = w->h;
                        continue;
                    }
                }

                /* Titlebar drag start? */
                if ((w->flags & WF_MOVEABLE) &&
                    mx >= wx && mx < wx + w->w &&
                    my >= wy && my < wy + WM_TITLEBAR_H) {
                    w->drag_active   = true;
                    w->drag_offset_x = mx - wx;
                    w->drag_offset_y = my - wy;
                    continue;
                }
            }

            if (evt.type == GUI_EVENT_MOUSE_UP) {
                /* End drag and resize on all windows */
                for (int i = 0; i < WM_MAX_WINDOWS; i++) {
                    if (window_used[i]) {
                        windows[i].drag_active   = false;
                        windows[i].resize_active = false;
                    }
                }
            }

            if (evt.type == GUI_EVENT_MOUSE_MOVE) {
                /* Move dragging windows */
                for (int i = 0; i < WM_MAX_WINDOWS; i++) {
                    if (!window_used[i]) continue;
                    window_t* w = &windows[i];

                    if (w->drag_active) {
                        int nx = mx - w->drag_offset_x;
                        int ny = my - w->drag_offset_y;
                        wm_move(w->id, nx, ny);
                    }

                    if (w->resize_active) {
                        int dx = mx - w->resize_start_mx;
                        int dy = my - w->resize_start_my;
                        int new_w = w->resize_start_w;
                        int new_h = w->resize_start_h;
                        int new_x = w->resize_start_x;
                        int new_y = w->resize_start_y;

                        if (w->resize_edge & RESIZE_RIGHT)  new_w += dx;
                        if (w->resize_edge & RESIZE_BOTTOM) new_h += dy;
                        if (w->resize_edge & RESIZE_LEFT) {
                            new_w -= dx; new_x += dx;
                        }
                        if (w->resize_edge & RESIZE_TOP) {
                            new_h -= dy; new_y += dy;
                        }

                        /* Enforce minimum size */
                        if (new_w < 120) new_w = 120;
                        if (new_h <  60) new_h =  60;

                        if (new_x != w->x || new_y != w->y)
                            wm_move(w->id, new_x, new_y);
                        if (new_w != w->w || new_h != w->h)
                            wm_resize(w->id, new_w, new_h);
                    }
                }
            }

            /* Forward to focused window's client area */
            if (wm_focused_wid >= 0) {
                window_t* fw = wm_get_window(wm_focused_wid);
                if (fw && fw->on_event) {
                    int client_x = mx - fw->x;
                    int client_y = my - (fw->y + WM_TITLEBAR_H);
                    gui_event_t fwd = evt;
                    fwd.mouse.x = client_x;
                    fwd.mouse.y = client_y;
                    fw->on_event(fw->id, &fwd, fw->userdata);
                }
            }

        } else if (evt.type == GUI_EVENT_KEY_DOWN || evt.type == GUI_EVENT_KEY_UP) {
            /* Forward keyboard events to focused window */
            if (wm_focused_wid >= 0) {
                window_t* fw = wm_get_window(wm_focused_wid);
                if (fw && fw->on_event)
                    fw->on_event(fw->id, &evt, fw->userdata);
            }
        }
    }
}

/* =========================================================
 * Compositor: blit all windows to screen canvas
 * ========================================================= */

/* Drop shadow constants */
#define SHADOW_OFFSET_X  4
#define SHADOW_OFFSET_Y  5
#define SHADOW_COLOR     rgba(0x00, 0x00, 0x00, 0x60)  /* 38% alpha */

void wm_composite(canvas_t* screen)
{
    /* Draw windows back-to-front */
    for (int i = 0; i < z_count; i++) {
        int s = z_order[i];
        if (!window_used[s]) continue;
        window_t* w = &windows[s];
        if (!(w->flags & WF_VISIBLE)) continue;
        if (w->flags & WF_MINIMIZED) continue;

        int win_total_h = WM_TITLEBAR_H + w->h;

        /* --- Drop shadow (translucent rect offset behind window) --- */
        int sx = w->x - WM_BORDER_W + SHADOW_OFFSET_X;
        int sy = w->y - WM_BORDER_W + SHADOW_OFFSET_Y;
        int sw = w->w + WM_BORDER_W * 2;
        int sh = win_total_h + WM_BORDER_W * 2;

        /* Clamp shadow to screen bounds */
        if (sx < 0) { sw += sx; sx = 0; }
        if (sy < 0) { sh += sy; sy = 0; }
        if (sx + sw > screen->width)  sw = screen->width  - sx;
        if (sy + sh > screen->height) sh = screen->height - sy;

        if (sw > 0 && sh > 0) {
            /* Blend shadow over whatever is behind */
            for (int row = 0; row < sh; row++) {
                for (int col = 0; col < sw; col++) {
                    int px = sx + col;
                    int py = sy + row;
                    if (px < 0 || px >= screen->width) continue;
                    if (py < 0 || py >= screen->height) continue;
                    uint32_t* dst = &screen->pixels[py * screen->stride + px];
                    *dst = fb_blend(*dst, SHADOW_COLOR);
                }
            }
        }

        /* --- Window border --- */
        draw_rect_outline(screen,
                          w->x - WM_BORDER_W,
                          w->y - WM_BORDER_W,
                          w->w + WM_BORDER_W * 2,
                          win_total_h + WM_BORDER_W * 2,
                          WM_BORDER_W,
                          (w->flags & WF_FOCUSED) ? COLOR_WIN_BORDER : COLOR_MID_GREY);

        /* --- Alpha-blend entire window buffer (titlebar + client) --- */
        draw_blit_alpha(screen, w->x, w->y, w->buf, w->w, win_total_h);

        /* --- Resize handles: small squares at corners/edges --- */
        if ((w->flags & WF_RESIZEABLE) && (w->flags & WF_FOCUSED)) {
            int rx = w->x + w->w - WM_RESIZE_ZONE;
            int ry = w->y + win_total_h - WM_RESIZE_ZONE;
            /* Bottom-right corner grip */
            draw_rect(screen, rx, ry, WM_RESIZE_ZONE, WM_RESIZE_ZONE,
                      COLOR_WIN_BORDER);
            /* Bottom-center edge mark */
            int bc_x = w->x + w->w / 2 - 3;
            draw_rect(screen, bc_x, w->y + win_total_h - 2, 6, 2,
                      COLOR_WIN_BORDER);
            /* Right-center edge mark */
            int rc_y = w->y + win_total_h / 2 - 3;
            draw_rect(screen, w->x + w->w - 2, rc_y, 2, 6,
                      COLOR_WIN_BORDER);
        }
    }
}
