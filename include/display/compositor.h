/*
 * include/display/compositor.h — Aether OS Compositor
 *
 * The Aether compositor manages display surfaces. Each window is a surface.
 * The compositor owns all framebuffer access — no application writes pixels
 * to the screen directly.
 *
 * Protocol: clients communicate with the compositor via IPC messages
 * (MSG_DISP_* types defined in ipc.h). The compositor runs as a kernel
 * thread registered as "aether.display" in the service bus.
 *
 * Key concepts:
 *   Surface   : an ARGB pixel buffer at a (x,y) position, with a Z-order
 *   Scene graph: list of surfaces sorted by Z (back to front)
 *   Damage    : the rectangle that changed since last composite
 *   Commit    : client signals "I am done drawing this frame, please show it"
 */
#ifndef DISPLAY_COMPOSITOR_H
#define DISPLAY_COMPOSITOR_H

#include <types.h>
#include <kernel/ipc.h>
#include <kernel/cap.h>
#include <gui/draw.h>

/* =========================================================
 * Surface flags
 * ========================================================= */

#define SURF_FLAG_VISIBLE    (1 << 0)
#define SURF_FLAG_FOCUSED    (1 << 1)
#define SURF_FLAG_MOVING     (1 << 2)
#define SURF_FLAG_RESIZING   (1 << 3)
#define SURF_FLAG_DECORATED  (1 << 4)  /* Draw titlebar + border */
#define SURF_FLAG_TOPMOST    (1 << 5)  /* Always on top (cursor, notifications) */
#define SURF_FLAG_FULLSCREEN (1 << 6)
#define SURF_FLAG_MINIMIZED  (1 << 7)

/* =========================================================
 * Surface (the fundamental display object)
 * ========================================================= */

#define COMP_MAX_SURFACES   64
#define COMP_TITLEBAR_H     28
#define COMP_BORDER_W        2
#define COMP_TITLE_LEN      128

typedef uint32_t surf_id_t;
#define SURF_INVALID  ((surf_id_t)0)

typedef struct {
    surf_id_t   id;
    char        title[COMP_TITLE_LEN];

    /* Geometry (screen coordinates, top-left of client area) */
    int         x, y;
    int         w, h;         /* Client area dimensions */

    /* Pixel buffers */
    uint32_t*   buf;          /* Allocated pixel buffer (w * (h + titlebar)) */
    canvas_t    client;       /* Canvas over the client region */
    canvas_t    full;         /* Canvas over the entire surface buffer */

    /* Capability for this surface's pixel buffer */
    cap_id_t    buf_cap;

    /* Z-ordering: higher = closer to viewer */
    int         z;

    /* Owner task's notification port (for input dispatch) */
    port_id_t   notify_port;
    uint32_t    owner_tid;

    /* Damage region (dirty rect since last composite) */
    int         dmg_x, dmg_y, dmg_w, dmg_h;
    bool        damaged;

    uint32_t    flags;
    bool        valid;

    /* Drag state (managed by compositor) */
    bool        drag_active;
    int         drag_off_x, drag_off_y;
} comp_surface_t;

/* =========================================================
 * Compositor state (exposed for service routines)
 * ========================================================= */

typedef struct {
    comp_surface_t   surfaces[COMP_MAX_SURFACES];
    uint32_t    surf_count;
    surf_id_t   focused_id;      /* Surface with keyboard focus */
    surf_id_t   next_id;

    canvas_t    screen;          /* Main back-buffer canvas */
    port_id_t   service_port;    /* "aether.display" IPC port */

    bool        initialized;
    bool        running;
} compositor_t;

extern compositor_t g_compositor;

/* =========================================================
 * Compositor API (called from the compositor kernel thread)
 * ========================================================= */

void       compositor_init(void);
void       compositor_run(void);         /* Main loop (compositor thread) */

/* Surface management */
surf_id_t  compositor_create_surface(uint32_t owner_tid, port_id_t notify_port,
                                      int x, int y, int w, int h,
                                      const char* title, uint32_t flags);
void       compositor_destroy_surface(surf_id_t id);
void       compositor_set_geometry(surf_id_t id, int x, int y, int w, int h);
void       compositor_set_visible(surf_id_t id, bool visible);
void       compositor_set_title(surf_id_t id, const char* title);
void       compositor_raise(surf_id_t id);
void       compositor_focus(surf_id_t id);
void       compositor_commit(surf_id_t id);  /* Mark surface as ready */
void       compositor_damage(surf_id_t id, int x, int y, int w, int h);

/* Get the client-area canvas for a surface */
canvas_t   compositor_get_canvas(surf_id_t id);

/* Composite all surfaces to the framebuffer back buffer, then flip */
void       compositor_composite(void);

/* Handle one incoming IPC message from the service port */
void       compositor_handle_msg(const ipc_msg_t* msg);

/* =========================================================
 * IPC message payload structures (for MSG_DISP_* messages)
 * ========================================================= */

/* MSG_DISP_CREATE request payload */
typedef struct {
    int      x, y, w, h;
    uint32_t flags;
    char     title[COMP_TITLE_LEN];
    port_id_t notify_port;   /* Caller's port for input events */
} msg_disp_create_t;

/* MSG_DISP_CREATE reply payload */
typedef struct {
    surf_id_t surf_id;
    cap_id_t  buf_cap;       /* Capability for the surface's pixel buffer */
    int       stride;        /* Pixels per row */
} msg_disp_create_reply_t;

/* MSG_DISP_SET_GEOM payload */
typedef struct {
    surf_id_t surf_id;
    int       x, y, w, h;
} msg_disp_geom_t;

/* MSG_DISP_SET_VIS payload */
typedef struct {
    surf_id_t surf_id;
    bool      visible;
} msg_disp_vis_t;

/* MSG_DISP_SET_TITLE payload */
typedef struct {
    surf_id_t surf_id;
    char      title[COMP_TITLE_LEN];
} msg_disp_title_t;

/* MSG_DISP_INFO reply payload */
typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
    uint32_t surf_count;
} msg_disp_info_t;

#endif /* DISPLAY_COMPOSITOR_H */
