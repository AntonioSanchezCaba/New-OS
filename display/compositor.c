/*
 * display/compositor.c — Aether OS Compositor Service
 *
 * The compositor is the authoritative owner of the framebuffer.
 * No application writes to the display directly; all rendering goes
 * through the compositor's surface API.
 *
 * Architecture:
 *   - Runs as a kernel thread registered as "aether.display"
 *   - Clients create surfaces via IPC (MSG_DISP_CREATE)
 *   - Each surface gets its own pixel buffer (allocated from buddy)
 *   - Damage tracking: only redraws dirty regions (TODO: hardware accel)
 *   - Composites back-to-front by Z-order each frame
 *   - Bridges to the existing gui/window_manager for legacy compat
 *
 * For v0.1 the compositor runs as a kernel thread and directly
 * calls the framebuffer driver.  True user-space isolation is v0.2+.
 */
#include <display/compositor.h>
#include <input/input_svc.h>
#include <kernel/ipc.h>
#include <kernel/cap.h>
#include <kernel/svcbus.h>
#include <gui/draw.h>
#include <gui/window.h>
#include <drivers/framebuffer.h>
#include <drivers/mouse.h>
#include <drivers/timer.h>
#include <memory.h>
#include <string.h>
#include <kernel.h>
#include <scheduler.h>

/* =========================================================
 * Global compositor state
 * ========================================================= */

compositor_t g_compositor;

/* =========================================================
 * Internal helpers
 * ========================================================= */

static surface_t* _find_surf(surf_id_t id)
{
    if (id == SURF_INVALID) return NULL;
    for (int i = 0; i < COMP_MAX_SURFACES; i++) {
        if (g_compositor.surfaces[i].valid &&
            g_compositor.surfaces[i].id == id)
            return &g_compositor.surfaces[i];
    }
    return NULL;
}

static surface_t* _alloc_surf(void)
{
    for (int i = 0; i < COMP_MAX_SURFACES; i++) {
        if (!g_compositor.surfaces[i].valid)
            return &g_compositor.surfaces[i];
    }
    return NULL;
}

/* Draw a decorated titlebar into the surface's full buffer */
static void _draw_titlebar(surface_t* s)
{
    if (!(s->flags & SURF_FLAG_DECORATED)) return;

    bool focused = (s->id == g_compositor.focused_id);
    uint32_t col_a = focused ? COLOR_WIN_TITLE : COLOR_MID_GREY;
    uint32_t col_b = focused ? rgb(0x1A, 0x3A, 0x6A) : COLOR_DARK_GREY;

    /* Gradient titlebar */
    draw_gradient_v(&s->full, 0, 0, s->w, COMP_TITLEBAR_H, col_a, col_b);

    /* Title text — centred, leaving room for the close button */
    draw_string_centered(&s->full, 0, 0, s->w - 28, COMP_TITLEBAR_H,
                         s->title, COLOR_TEXT_LIGHT, rgba(0,0,0,0));

    /* Close button */
    int bx = s->w - 24;
    draw_rect_rounded(&s->full, bx, 5, 18, 18, 4, COLOR_BTN_CLOSE);
    draw_string(&s->full, bx + 5, 8, "x", COLOR_TEXT_LIGHT, rgba(0,0,0,0));

    /* Bottom separator */
    draw_hline(&s->full, 0, COMP_TITLEBAR_H - 1, s->w, COLOR_WIN_BORDER);
}

/* Sort surfaces by Z (insertion sort — small array) */
static void _sort_by_z(int* order, int n)
{
    for (int i = 1; i < n; i++) {
        int key = order[i];
        int j   = i - 1;
        while (j >= 0 &&
               g_compositor.surfaces[order[j]].z >
               g_compositor.surfaces[key].z) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = key;
    }
}

/* =========================================================
 * Initialisation
 * ========================================================= */

void compositor_init(void)
{
    memset(&g_compositor, 0, sizeof(g_compositor));
    g_compositor.next_id     = 1;
    g_compositor.focused_id  = SURF_INVALID;
    g_compositor.initialized = false;
    g_compositor.running     = false;

    if (!fb_ready()) {
        klog_warn("COMP: framebuffer not ready — compositor disabled");
        return;
    }

    g_compositor.screen = draw_main_canvas();

    /* Create the compositor's IPC service port */
    g_compositor.service_port = ipc_port_create(0 /* kernel tid */);
    if (g_compositor.service_port == PORT_INVALID) {
        klog_warn("COMP: failed to create service port");
        return;
    }

    /* Register in the service bus */
    svcbus_register(SVC_DISPLAY, g_compositor.service_port, 0, 1);

    g_compositor.initialized = true;

    kinfo("COMP: compositor ready — %ux%u display, service port %u",
          g_compositor.screen.width,
          g_compositor.screen.height,
          g_compositor.service_port);
}

/* =========================================================
 * Surface management
 * ========================================================= */

surf_id_t compositor_create_surface(uint32_t owner_tid, port_id_t notify_port,
                                     int x, int y, int w, int h,
                                     const char* title, uint32_t flags)
{
    if (w <= 0 || h <= 0) return SURF_INVALID;

    surface_t* s = _alloc_surf();
    if (!s) {
        klog_warn("COMP: surface table full");
        return SURF_INVALID;
    }

    /* Pixel buffer: full height includes titlebar if decorated */
    int total_h = (flags & SURF_FLAG_DECORATED) ? h + COMP_TITLEBAR_H : h;
    size_t buf_bytes = (size_t)w * (size_t)total_h * sizeof(uint32_t);

    uint32_t* buf = (uint32_t*)kmalloc(buf_bytes);
    if (!buf) {
        klog_warn("COMP: kmalloc failed for surface %ux%u", w, h);
        return SURF_INVALID;
    }
    memset(buf, 0, buf_bytes);

    s->id          = g_compositor.next_id++;
    s->x           = x;
    s->y           = y;
    s->w           = w;
    s->h           = h;
    s->buf         = buf;
    s->flags       = flags | SURF_FLAG_VISIBLE;
    s->notify_port = notify_port;
    s->owner_tid   = owner_tid;
    s->z           = (int)g_compositor.surf_count;
    s->damaged     = true;
    s->dmg_x       = 0; s->dmg_y = 0; s->dmg_w = w; s->dmg_h = h;
    s->valid       = true;
    s->drag_active = false;

    if (title) {
        strncpy(s->title, title, COMP_TITLE_LEN - 1);
        s->title[COMP_TITLE_LEN - 1] = '\0';
    }

    /* Full-buffer canvas */
    s->full.pixels = buf;
    s->full.width  = w;
    s->full.height = total_h;
    s->full.stride = w;

    /* Client canvas — starts after titlebar */
    if (flags & SURF_FLAG_DECORATED) {
        s->client.pixels = buf + (size_t)COMP_TITLEBAR_H * (size_t)w;
    } else {
        s->client.pixels = buf;
    }
    s->client.width  = w;
    s->client.height = h;
    s->client.stride = w;

    /* Capability for the pixel buffer */
    s->buf_cap = cap_create(CAP_TYPE_DISPLAY,
                            CAP_RIGHT_READ | CAP_RIGHT_WRITE | CAP_RIGHT_MAP,
                            buf, owner_tid);

    /* Fill client area with window background */
    draw_rect(&s->client, 0, 0, w, h, COLOR_WIN_BG);

    /* Draw initial titlebar */
    _draw_titlebar(s);

    g_compositor.surf_count++;

    /* Auto-focus new surface */
    compositor_focus(s->id);

    kinfo("COMP: surface %u created '%s' %dx%d at (%d,%d) owner=%u",
          s->id, s->title, w, h, x, y, owner_tid);
    return s->id;
}

void compositor_destroy_surface(surf_id_t id)
{
    surface_t* s = _find_surf(id);
    if (!s) return;

    if (s->buf_cap != CAP_INVALID_ID) cap_release(s->buf_cap);
    if (s->buf) kfree(s->buf);

    s->valid = false;
    s->buf   = NULL;
    g_compositor.surf_count--;

    if (g_compositor.focused_id == id) {
        g_compositor.focused_id = SURF_INVALID;
        /* Focus the topmost remaining surface */
        int   best_z  = -1;
        surf_id_t best = SURF_INVALID;
        for (int i = 0; i < COMP_MAX_SURFACES; i++) {
            if (g_compositor.surfaces[i].valid &&
                g_compositor.surfaces[i].z > best_z) {
                best_z = g_compositor.surfaces[i].z;
                best   = g_compositor.surfaces[i].id;
            }
        }
        if (best != SURF_INVALID) compositor_focus(best);
    }
}

void compositor_set_geometry(surf_id_t id, int x, int y, int w, int h)
{
    surface_t* s = _find_surf(id);
    if (!s) return;
    s->x = x; s->y = y;
    /* Resize is more involved (realloc buffer) — just move for now */
    (void)w; (void)h;
    s->damaged = true;
    s->dmg_x = 0; s->dmg_y = 0; s->dmg_w = s->w; s->dmg_h = s->h;
}

void compositor_set_visible(surf_id_t id, bool visible)
{
    surface_t* s = _find_surf(id);
    if (!s) return;
    if (visible) {
        s->flags |=  SURF_FLAG_VISIBLE;
        s->flags &= ~SURF_FLAG_MINIMIZED;
    } else {
        s->flags &= ~SURF_FLAG_VISIBLE;
    }
}

void compositor_set_title(surf_id_t id, const char* title)
{
    surface_t* s = _find_surf(id);
    if (!s || !title) return;
    strncpy(s->title, title, COMP_TITLE_LEN - 1);
    s->title[COMP_TITLE_LEN - 1] = '\0';
    _draw_titlebar(s);
    s->damaged = true;
}

void compositor_raise(surf_id_t id)
{
    surface_t* s = _find_surf(id);
    if (!s) return;
    /* Find max Z and set this surface above it */
    int max_z = 0;
    for (int i = 0; i < COMP_MAX_SURFACES; i++) {
        if (g_compositor.surfaces[i].valid &&
            g_compositor.surfaces[i].z > max_z)
            max_z = g_compositor.surfaces[i].z;
    }
    s->z = max_z + 1;
}

void compositor_focus(surf_id_t id)
{
    /* Unfocus old */
    surface_t* old = _find_surf(g_compositor.focused_id);
    if (old) {
        old->flags &= ~SURF_FLAG_FOCUSED;
        _draw_titlebar(old);
        old->damaged = true;
    }

    g_compositor.focused_id = id;

    surface_t* s = _find_surf(id);
    if (s) {
        s->flags |= SURF_FLAG_FOCUSED;
        _draw_titlebar(s);
        s->damaged = true;
        compositor_raise(id);
    }
}

void compositor_commit(surf_id_t id)
{
    surface_t* s = _find_surf(id);
    if (!s) return;
    s->damaged = true;
    s->dmg_x = 0; s->dmg_y = 0;
    s->dmg_w = s->w; s->dmg_h = s->h;
}

void compositor_damage(surf_id_t id, int x, int y, int w, int h)
{
    surface_t* s = _find_surf(id);
    if (!s) return;
    /* Expand damage rect to union */
    if (!s->damaged) {
        s->dmg_x = x; s->dmg_y = y;
        s->dmg_w = w; s->dmg_h = h;
    } else {
        int x1 = s->dmg_x + s->dmg_w;
        int y1 = s->dmg_y + s->dmg_h;
        int nx1 = x + w, ny1 = y + h;
        if (x  < s->dmg_x) s->dmg_x = x;
        if (y  < s->dmg_y) s->dmg_y = y;
        if (nx1 > x1)      s->dmg_w = nx1 - s->dmg_x;
        if (ny1 > y1)      s->dmg_h = ny1 - s->dmg_y;
    }
    s->damaged = true;
}

canvas_t compositor_get_canvas(surf_id_t id)
{
    surface_t* s = _find_surf(id);
    if (!s) {
        canvas_t empty = {NULL, 0, 0, 0};
        return empty;
    }
    return s->client;
}

/* =========================================================
 * compositor_composite — draw all surfaces to back buffer, then flip
 * ========================================================= */

void compositor_composite(void)
{
    canvas_t* screen = &g_compositor.screen;

    /* Build Z-sorted index */
    int order[COMP_MAX_SURFACES];
    int n = 0;
    for (int i = 0; i < COMP_MAX_SURFACES; i++) {
        if (g_compositor.surfaces[i].valid)
            order[n++] = i;
    }
    _sort_by_z(order, n);

    /* Paint desktop background */
    draw_gradient_v(screen, 0, 0,
                    screen->width,
                    screen->height,
                    COLOR_DESKTOP_BG,
                    rgb(0x0C, 0x14, 0x24));

    /* Subtle grid overlay */
    uint32_t grid = rgba(0xFF, 0xFF, 0xFF, 0x08);
    for (int gx = 0; gx < screen->width;  gx += 40)
        draw_vline(screen, gx, 0, screen->height, grid);
    for (int gy = 0; gy < screen->height; gy += 40)
        draw_hline(screen, 0, gy, screen->width,  grid);

    /* Composite each visible surface back-to-front */
    for (int i = 0; i < n; i++) {
        surface_t* s = &g_compositor.surfaces[order[i]];
        if (!s->valid)                      continue;
        if (!(s->flags & SURF_FLAG_VISIBLE)) continue;
        if (s->flags & SURF_FLAG_MINIMIZED) continue;

        int total_h = (s->flags & SURF_FLAG_DECORATED)
                      ? s->h + COMP_TITLEBAR_H : s->h;

        /* Draw border */
        bool focused = (s->id == g_compositor.focused_id);
        draw_rect_outline(screen,
                          s->x - COMP_BORDER_W,
                          s->y - COMP_BORDER_W,
                          s->w + COMP_BORDER_W * 2,
                          total_h + COMP_BORDER_W * 2,
                          COMP_BORDER_W,
                          focused ? COLOR_WIN_BORDER : COLOR_MID_GREY);

        /* Blit surface buffer */
        draw_blit(screen, s->x, s->y, s->buf, s->w, total_h);

        s->damaged = false;
    }

    /* Draw mouse cursor on top */
    mouse_draw_cursor(fb.back_buf, (int)fb.width, (int)fb.height);

    /* Flip back buffer to physical framebuffer */
    fb_flip();
}

/* =========================================================
 * IPC message handler
 * ========================================================= */

void compositor_handle_msg(const ipc_msg_t* msg)
{
    ipc_msg_t reply;
    memset(&reply, 0, sizeof(reply));
    reply.sender_tid = 0;

    switch (msg->type) {

    case MSG_DISP_INFO: {
        msg_disp_info_t* info = (msg_disp_info_t*)reply.data;
        info->width      = (uint32_t)g_compositor.screen.width;
        info->height     = (uint32_t)g_compositor.screen.height;
        info->bpp        = 32;
        info->surf_count = g_compositor.surf_count;
        reply.data_len   = sizeof(msg_disp_info_t);
        reply.type       = MSG_DISP_INFO;
        ipc_reply(msg, &reply);
        break;
    }

    case MSG_DISP_CREATE: {
        if (msg->data_len < sizeof(msg_disp_create_t)) break;
        msg_disp_create_t* req = (msg_disp_create_t*)msg->data;

        surf_id_t sid = compositor_create_surface(
            msg->sender_tid,
            req->notify_port,
            req->x, req->y, req->w, req->h,
            req->title, req->flags);

        msg_disp_create_reply_t* rep = (msg_disp_create_reply_t*)reply.data;
        rep->surf_id = sid;
        rep->stride  = (sid != SURF_INVALID) ? req->w : 0;
        surface_t* s = _find_surf(sid);
        rep->buf_cap = (s && sid != SURF_INVALID) ? s->buf_cap : CAP_INVALID_ID;
        reply.data_len = sizeof(msg_disp_create_reply_t);
        reply.type     = MSG_DISP_CREATE;
        ipc_reply(msg, &reply);
        break;
    }

    case MSG_DISP_DESTROY: {
        surf_id_t* sid = (surf_id_t*)msg->data;
        compositor_destroy_surface(*sid);
        reply.type = MSG_DISP_DESTROY;
        ipc_reply(msg, &reply);
        break;
    }

    case MSG_DISP_SET_GEOM: {
        if (msg->data_len < sizeof(msg_disp_geom_t)) break;
        msg_disp_geom_t* g = (msg_disp_geom_t*)msg->data;
        compositor_set_geometry(g->surf_id, g->x, g->y, g->w, g->h);
        break;
    }

    case MSG_DISP_SET_VIS: {
        if (msg->data_len < sizeof(msg_disp_vis_t)) break;
        msg_disp_vis_t* v = (msg_disp_vis_t*)msg->data;
        compositor_set_visible(v->surf_id, v->visible);
        break;
    }

    case MSG_DISP_SET_TITLE: {
        if (msg->data_len < sizeof(msg_disp_title_t)) break;
        msg_disp_title_t* t = (msg_disp_title_t*)msg->data;
        compositor_set_title(t->surf_id, t->title);
        break;
    }

    case MSG_DISP_COMMIT: {
        surf_id_t* sid = (surf_id_t*)msg->data;
        compositor_commit(*sid);
        break;
    }

    case MSG_DISP_RAISE: {
        surf_id_t* sid = (surf_id_t*)msg->data;
        compositor_raise(*sid);
        compositor_focus(*sid);
        break;
    }

    default:
        klog_warn("COMP: unknown message type 0x%x", msg->type);
        break;
    }
}

/* =========================================================
 * Mouse / drag handling
 * ========================================================= */

static void _handle_mouse(int mx, int my, uint8_t buttons, uint8_t prev_btns)
{
    bool pressed  = (buttons  & MOUSE_BTN_LEFT) != 0;
    bool was_down = (prev_btns & MOUSE_BTN_LEFT) != 0;
    bool clicked  = pressed && !was_down;
    bool released = !pressed && was_down;

    /* End drag on release */
    if (released) {
        for (int i = 0; i < COMP_MAX_SURFACES; i++) {
            if (g_compositor.surfaces[i].valid)
                g_compositor.surfaces[i].drag_active = false;
        }
    }

    /* Continue drag */
    if (pressed) {
        for (int i = 0; i < COMP_MAX_SURFACES; i++) {
            surface_t* s = &g_compositor.surfaces[i];
            if (!s->valid || !s->drag_active) continue;
            int nx = mx - s->drag_off_x;
            int ny = my - s->drag_off_y;
            if (ny < 0) ny = 0;
            s->x = nx; s->y = ny;
        }
    }

    if (!clicked) return;

    /* Hit test front-to-back (highest Z first) */
    int best_z = -1;
    surface_t* hit = NULL;

    for (int i = 0; i < COMP_MAX_SURFACES; i++) {
        surface_t* s = &g_compositor.surfaces[i];
        if (!s->valid || !(s->flags & SURF_FLAG_VISIBLE)) continue;
        if (s->flags & SURF_FLAG_MINIMIZED) continue;

        int total_h = (s->flags & SURF_FLAG_DECORATED)
                      ? s->h + COMP_TITLEBAR_H : s->h;

        if (mx >= s->x && mx < s->x + s->w &&
            my >= s->y && my < s->y + total_h &&
            s->z > best_z) {
            best_z = s->z;
            hit    = s;
        }
    }

    if (!hit) return;

    /* Focus and raise */
    compositor_focus(hit->id);

    /* Close button? */
    if ((hit->flags & SURF_FLAG_DECORATED) &&
        mx >= hit->x + hit->w - 24 && mx < hit->x + hit->w - 6 &&
        my >= hit->y + 5           && my < hit->y + 23) {
        /* Send close event to owner */
        if (hit->notify_port != PORT_INVALID) {
            ipc_msg_t ev;
            memset(&ev, 0, sizeof(ev));
            ev.type       = MSG_TYPE_SYSTEM;
            ev.sender_tid = 0;
            ev.data_len   = 0;
            ipc_send(hit->notify_port, &ev);
        }
        compositor_destroy_surface(hit->id);
        return;
    }

    /* Titlebar drag? */
    if ((hit->flags & SURF_FLAG_DECORATED) &&
        my >= hit->y && my < hit->y + COMP_TITLEBAR_H) {
        hit->drag_active = true;
        hit->drag_off_x  = mx - hit->x;
        hit->drag_off_y  = my - hit->y;
        return;
    }

    /* Forward click to window's notify port */
    if (hit->notify_port != PORT_INVALID) {
        ipc_msg_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = MSG_INPUT_MOUSE_BTN;
        ev.sender_tid = 0;
        input_mouse_btn_t* btn = (input_mouse_btn_t*)ev.data;
        btn->x       = mx - hit->x;
        btn->y       = my - (hit->y + ((hit->flags & SURF_FLAG_DECORATED)
                                        ? COMP_TITLEBAR_H : 0));
        btn->buttons = buttons;
        btn->pressed = true;
        ev.data_len  = sizeof(input_mouse_btn_t);
        ipc_send(hit->notify_port, &ev);
    }
}

/* =========================================================
 * compositor_run — compositor main loop (kernel thread entry)
 * ========================================================= */

void compositor_run(void)
{
    if (!g_compositor.initialized) {
        klog_warn("COMP: not initialized — thread exiting");
        return;
    }

    g_compositor.running = true;
    kinfo("COMP: compositor thread running");

    uint32_t    last_frame = 0;
    uint8_t     prev_btns  = 0;
    ipc_msg_t   msg;

    while (g_compositor.running) {
        uint32_t now = timer_get_ticks();

        /* Process incoming IPC messages (non-blocking) */
        while (ipc_receive(g_compositor.service_port, &msg,
                           IPC_TIMEOUT_NONE) == IPC_OK) {
            compositor_handle_msg(&msg);
        }

        /* Handle mouse input */
        uint8_t cur_btns = mouse.buttons;
        _handle_mouse(mouse.x, mouse.y, cur_btns, prev_btns);
        prev_btns = cur_btns;

        /* Throttle to ~60fps (approx 16ms at 1000Hz, 8 ticks at 500Hz) */
        if (now - last_frame >= 2) {
            compositor_composite();
            last_frame = now;
        }

        scheduler_yield();
    }
}
