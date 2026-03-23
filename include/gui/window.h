/*
 * include/gui/window.h - Window manager structures and API
 *
 * Each window is an independently buffered surface that the compositor
 * blends onto the desktop. The window manager handles:
 *   - Z-ordering (front/back stacking)
 *   - Title bars and close/minimize buttons
 *   - Dragging via the title bar
 *   - Mouse focus dispatch
 *   - Keyboard focus dispatch
 */
#ifndef GUI_WINDOW_H
#define GUI_WINDOW_H

#include <types.h>
#include <gui/draw.h>
#include <gui/event.h>

/* Maximum windows on screen */
#define WM_MAX_WINDOWS 32

/* Title bar height in pixels */
#define WM_TITLEBAR_H  28

/* Window border thickness */
#define WM_BORDER_W    2

/* Window flags */
#define WF_VISIBLE     (1 << 0)
#define WF_FOCUSED     (1 << 1)
#define WF_MOVEABLE    (1 << 2)
#define WF_CLOSEABLE   (1 << 3)
#define WF_MINIMIZED   (1 << 4)
#define WF_MAXIMIZED   (1 << 5)
#define WF_NO_TITLEBAR (1 << 6)
#define WF_RESIZEABLE  (1 << 7)   /* Allows drag-to-resize at edges/corners */
#define WF_SHADED      (1 << 8)   /* Window rolled up to title bar only     */

/* Resize edge bitmask (stored in resize_edge) */
#define RESIZE_NONE   0x00
#define RESIZE_RIGHT  0x01
#define RESIZE_BOTTOM 0x02
#define RESIZE_LEFT   0x04
#define RESIZE_TOP    0x08

/* Handle grab zone thickness in pixels */
#define WM_RESIZE_ZONE 6

/* Window identifier */
typedef int wid_t;

/* Application event callback */
typedef void (*window_event_fn)(wid_t wid, gui_event_t* evt, void* userdata);

/* Window descriptor */
typedef struct {
    wid_t    id;
    char     title[128];
    int      x, y;         /* Position on desktop */
    int      w, h;         /* Client area dimensions (excluding titlebar) */
    uint32_t flags;

    uint32_t* buf;          /* Pixel buffer (w * (h + WM_TITLEBAR_H)) */
    canvas_t  canvas;       /* Canvas over the client area */

    /* Application event handler */
    window_event_fn on_event;
    void*           userdata;

    /* Internal drag state */
    bool drag_active;
    int  drag_offset_x;
    int  drag_offset_y;

    /* Internal resize state */
    bool     resize_active;
    uint8_t  resize_edge;     /* RESIZE_* bitmask */
    int      resize_start_mx; /* Mouse position at resize start */
    int      resize_start_my;
    int      resize_start_x;  /* Window geometry at resize start */
    int      resize_start_y;
    int      resize_start_w;
    int      resize_start_h;

    /* Pre-minimize/maximize saved geometry */
    int saved_x, saved_y, saved_w, saved_h;
} window_t;

/* Window manager API */
void    wm_init(void);
wid_t   wm_create_window(const char* title, int x, int y, int w, int h,
                          window_event_fn cb, void* userdata);
void    wm_destroy_window(wid_t wid);
window_t* wm_get_window(wid_t wid);
void    wm_show(wid_t wid);
void    wm_hide(wid_t wid);
void    wm_focus(wid_t wid);
void    wm_move(wid_t wid, int x, int y);
void    wm_resize(wid_t wid, int w, int h);
void    wm_raise(wid_t wid);           /* Bring to front */
void    wm_close(wid_t wid);

/* Called by compositor each frame */
void    wm_dispatch_events(void);     /* Process mouse/keyboard into windows */
void    wm_composite(canvas_t* screen); /* Draw all windows to screen canvas */

/* Redraw a window's content (calls its on_event with PAINT) */
void    wm_invalidate(wid_t wid);
void    wm_invalidate_all(void);

/* Utility: create a canvas pointing to window client area */
canvas_t wm_client_canvas(wid_t wid);

/* The currently focused window */
extern wid_t wm_focused_wid;

#endif /* GUI_WINDOW_H */
